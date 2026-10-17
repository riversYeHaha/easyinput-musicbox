#include "wifi_share.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "songbook.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi_share";

/* 热点参数（项目私有合同，可在此调整） */
#define AP_CHANNEL        1
#define AP_MAX_CONN       4
#define IDLE_AUTO_OFF_MS  (10 * 60 * 1000) /* 10 分钟无 HTTP 访问自动关热点省电 */
#define WATCHDOG_PERIOD_MS 5000

static esp_netif_t    *s_ap_netif;
static httpd_handle_t  s_httpd;
static TaskHandle_t    s_dns_task;
static bool            s_enabled;
static char            s_ssid[32];
static volatile int64_t s_last_http_ms;  /* 最近一次 HTTP 请求时间（esp_timer ms） */

/* ---------- 依赖初始化（幂等，Wi-Fi 必须的 NVS/netif/event_loop） ---------- */

static void ensure_system_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        (void)nvs_flash_init();
    }
    (void)esp_netif_init();                 /* 重复调用返回 ESP_OK */
    (void)esp_event_loop_create_default();  /* 已存在返回 ESP_ERR_INVALID_STATE，忽略 */
}

/* ---------- 极简 DNS 服务器：所有 A 查询都应答 192.168.4.1（Captive Portal） ---------- */

static void dns_server_task(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0) {
        ESP_LOGE(TAG, "dns socket failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in srv = {
        .sin_family = AF_INET,
        .sin_port   = htons(53),
        .sin_addr   = { .s_addr = htonl(INADDR_ANY) },
    };
    if (bind(fd, (struct sockaddr *)&srv, sizeof(srv)) != 0) {
        ESP_LOGE(TAG, "dns bind failed: errno %d", errno);
        close(fd);
        vTaskDelete(NULL);
        return;
    }
    const uint8_t ip[4] = { 192, 168, 4, 1 };
    uint8_t buf[512];
    while (s_enabled) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n < 12) {
            continue;
        }
        if (buf[2] & 0x80) { /* 只应答查询（QR=0） */
            continue;
        }
        uint8_t resp[512];
        if (n > (int)sizeof(resp) - 16) {
            continue;
        }
        memcpy(resp, buf, (size_t)n);
        resp[2] = 0x84; /* QR=1, AA=1 */
        resp[3] = 0x00;
        /* ancount = qdcount */
        resp[6] = buf[4];
        resp[7] = buf[5];
        /* 跳过 question 段（到 0x00 终止 + 4 字节 QTYPE/QCLASS） */
        int i = 12;
        while (i < n && buf[i] != 0) {
            i += buf[i] + 1;
        }
        i += 5; /* null + QTYPE(2) + QCLASS(2) */
        /* answer：name pointer(0xc00c) + type A + class IN + ttl + rdlength + ip */
        uint8_t ans[] = {
            0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x3c,               /* TTL 60s */
            0x00, 0x04, ip[0], ip[1], ip[2], ip[3],
        };
        memcpy(resp + n, ans, sizeof(ans));
        sendto(fd, resp, n + sizeof(ans), 0, (struct sockaddr *)&from, flen);
    }
    close(fd);
    vTaskDelete(NULL);
}

/* ---------- HTTP 处理函数 ---------- */

static void touch_http(void)
{
    s_last_http_ms = esp_timer_get_time() / 1000;
}

static esp_err_t send_index(httpd_req_t *req)
{
    touch_http();

    /* 缓冲区必须走堆：httpd 任务栈有限（本工程配 6KB），大数组上栈会直接溢出。
     * 4096 覆盖 4 首歌 × ~200B 块 + ~1.5KB CSS/JS 头尾，仍留余量。 */
    const size_t cap = 4096;
    char *body = malloc(cap);
    if (body == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    int off = 0;
    off += snprintf(body + off, cap - off,
        "<!DOCTYPE html><html lang='zh'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>MusicBox 取歌</title>"
        "<style>body{font-family:-apple-system,sans-serif;max-width:640px;margin:0 auto;"
        "padding:20px;background:#0e0e12;color:#eee}"
        "h1{font-size:20px}"
        ".s{margin:12px 0;padding:14px;background:#1c1c24;border-radius:10px}"
        ".t{font-weight:600;margin-bottom:4px;word-break:break-all}"
        ".m{font-size:13px;color:#9a9aa8}"
        ".btns{display:flex;gap:10px;margin-top:10px}"
        ".p,.d{flex:1;padding:9px 12px;border-radius:8px;text-align:center;"
        "text-decoration:none;font-size:14px;border:0;color:#fff;cursor:pointer;"
        "font-family:inherit}"
        ".p{background:#3b82f6}.p.playing{background:#f59e0b}"
        ".d{background:#22c55e}"
        "audio{width:100%%;margin-top:10px}</style></head><body>"
        "<h1>🎵 MusicBox 歌曲</h1>"
        "<p style='color:#9a9aa8'>播放按钮：就地试听；下载按钮：保存到手机文件。</p>");

    int shown = 0;
    for (int i = 0; i < SONGBOOK_SLOTS; i++) {
        const char *title = songbook_get_title(i);
        if (title == NULL) {
            continue;
        }
        uint32_t sz = songbook_get_size(i);
        uint32_t dur = songbook_get_duration(i);
        char meta[64];
        if (dur > 0) {
            snprintf(meta, sizeof(meta), "%lu:%02lu  ·  %lu KB",
                     (unsigned long)(dur / 60), (unsigned long)(dur % 60),
                     (unsigned long)((sz + 1023) / 1024));
        } else {
            snprintf(meta, sizeof(meta), "%lu KB", (unsigned long)((sz + 1023) / 1024));
        }
        off += snprintf(body + off, cap - off,
            "<div class='s'>"
            "<div class='t'>%s</div><div class='m'>%s</div>"
            "<div class='btns'>"
            "<button class='p' data-src='/song/%d.mp3'>▶ 播放</button>"
            "<a class='d' href='/song/%d.mp3?dl=1' download>⬇ 下载</a>"
            "</div>"
            "<audio controls preload='none' playsinline hidden></audio>"
            "</div>",
            title, meta, i, i);
        shown++;
    }
    if (shown == 0) {
        off += snprintf(body + off, cap - off,
            "<p>暂无歌曲，请先用电脑端通过按键 S1/S2 生成歌曲。</p>");
    }

    /* 极简播放器控制脚本：
     * - 首次点击播放：把 data-src 赋给 audio.src、取消 hidden、调 play()
     * - 已在播的 audio 再点播放：pause
     * - 播放时暂停兄弟 audio（同一页只允许一首在响）
     * - play/pause/ended 事件同步按钮文案与 .playing 类
     * iOS Safari 支持 closest/forEach/getAttribute，无需前缀。 */
    off += snprintf(body + off, cap - off,
        "<script>(function(){"
        "var btns=document.querySelectorAll('.p');"
        "function label(b,a){ if(a.paused||a.ended){b.textContent='▶ 播放';b.classList.remove('playing');}else{b.textContent='⏸ 暂停';b.classList.add('playing');} }"
        "btns.forEach(function(b){"
        "  var w=b.closest('.s'); if(!w) return;"
        "  var a=w.querySelector('audio'); if(!a) return;"
        "  b.addEventListener('click',function(){"
        "    if(!a.getAttribute('src')){ a.src=b.getAttribute('data-src'); a.load(); }"
        "    a.removeAttribute('hidden');"
        "    if(a.paused){"
        "      document.querySelectorAll('audio').forEach(function(o){ if(o!==a) o.pause(); });"
        "      var pr=a.play(); if(pr&&pr.catch){ pr.catch(function(){ b.textContent='⚠ 再点一次'; }); }"
        "    } else { a.pause(); }"
        "  });"
        "  a.addEventListener('play',function(){label(b,a);});"
        "  a.addEventListener('pause',function(){label(b,a);});"
        "  a.addEventListener('ended',function(){label(b,a);});"
        "});"
        "})();</script>");

    off += snprintf(body + off, cap - off, "</body></html>");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t ret = httpd_resp_send(req, body, (size_t)off);
    free(body);
    return ret;
}

/* 从 UTF-8 标题提取 ASCII slug：只保留字母数字，其余字节（中文/空格/标点）
 * 折叠为单个 '-' 分隔，且首尾不留 '-'。用于 Option 3 的纯英文下载文件名——
 * 英文/数字标题能带出辨识度，纯中文标题则 slug 为空，调用方回落 song-N.mp3。
 * 页面列表仍显示原始中文标题，下载文件名只用于保存后的文件名。 */
static void ascii_slug(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    bool pending_dash = false;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < outsz; p++) {
        unsigned char c = *p;
        int is_alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                       (c >= '0' && c <= '9');
        if (is_alnum) {
            if (pending_dash && o > 0) {
                if (o + 1 >= outsz) {
                    break;
                }
                out[o++] = '-';
            }
            pending_dash = false;
            out[o++] = (char)c;
        } else if (o > 0) { /* 词间分隔；开头非字母数字不产生 '-' */
            pending_dash = true;
        }
    }
    out[o] = '\0';
}

/* GET /song/{n}.mp3：流式下载，支持 Range（iOS 音频与断点续传） */
static esp_err_t song_get_handler(httpd_req_t *req)
{
    touch_http();

    int index = -1;
    if (sscanf(req->uri, "/song/%d.mp3", &index) != 1 ||
        index < 0 || index >= SONGBOOK_SLOTS || songbook_get_title(index) == NULL) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req, "not found", HTTPD_RESP_USE_STRLEN);
    }

    uint32_t total = songbook_get_size(index);

    /* ?dl=1 仅由“下载”链接携带：触发附件式强制下载；“播放”链接不带此参数，
     * 保持内联流式播放。两条链接指向同一 URL 前缀，用查询串区分用途。 */
    bool want_download = false;
    {
        char q[64];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            want_download = (strstr(q, "dl=1") != NULL);
        }
    }

    /* 解析 Range: bytes=start- */
    uint32_t start = 0;
    bool partial = false;
    char range[64];
    if (httpd_req_get_hdr_value_str(req, "Range", range, sizeof(range)) == ESP_OK) {
        long s = 0;
        if (sscanf(range, "bytes=%ld-", &s) == 1 && s >= 0 && (uint32_t)s < total) {
            start = (uint32_t)s;
            partial = true;
        }
    }

    /* 下载必须拿到完整文件：忽略 Range，强制整段传输，避免只存下半截 */
    if (want_download) {
        start = 0;
        partial = false;
    }

    httpd_resp_set_type(req, "audio/mpeg");
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    if (want_download) {
        /* Option 3：下载名走纯 ASCII（标题提取 slug，无则回落 song-N.mp3），
         * 保证任意手机浏览器都能正确命名；页面列表仍显示中文标题。 */
        char slug[SONGBOOK_TITLE_MAX + 1];
        ascii_slug(songbook_get_title(index), slug, sizeof(slug));
        char disp[128];
        if (slug[0] != '\0') {
            snprintf(disp, sizeof(disp),
                     "attachment; filename=\"song-%d-%s.mp3\"", index, slug);
        } else {
            snprintf(disp, sizeof(disp),
                     "attachment; filename=\"song-%d.mp3\"", index);
        }
        httpd_resp_set_hdr(req, "Content-Disposition", disp);
    }

    /* 关键：必须给出精确 Content-Length。此前用 chunked 且无长度，
     * Safari（macOS/iOS）会拒绝起播 <audio>、并把 ?dl=1 当内联媒体在新标签打开
     * 而不是弹下载器；Chrome 宽松才侥幸可用。total 已知，直接声明定长。 */
    uint32_t send_len = total - start;
    {
        char lenstr[16];
        snprintf(lenstr, sizeof(lenstr), "%lu", (unsigned long)send_len);
        httpd_resp_set_hdr(req, "Content-Length", lenstr);
    }
    ESP_LOGI(TAG, "GET %s q=%s dl=%d range=%s total=%lu",
             req->uri,
             (httpd_req_get_url_query_str(req, (char[64]){0}, 64) == ESP_OK) ? "y" : "n",
             (int)want_download,
             partial ? "y" : "n",
             (unsigned long)total);
    if (partial) {
        char cr[64];
        snprintf(cr, sizeof(cr), "bytes %lu-%lu/%lu",
                 (unsigned long)start, (unsigned long)(total - 1), (unsigned long)total);
        httpd_resp_set_status(req, "206 Partial Content");
        httpd_resp_set_hdr(req, "Content-Range", cr);
    }

    /* 定长分块发送；8KB 缓冲区走堆，httpd 任务栈放不下 */
    const size_t chunk_cap = 8192;
    uint8_t *chunk = malloc(chunk_cap);
    if (chunk == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    uint32_t pos = start;
    while (pos < total) {
        size_t want = chunk_cap;
        if (pos + want > total) {
            want = total - pos;
        }
        size_t got = 0;
        if (songbook_read(index, pos, chunk, want, &got) != ESP_OK || got == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, (const char *)chunk, got) != ESP_OK) {
            break; /* 客户端断开 */
        }
        pos += (uint32_t)got;
    }
    free(chunk);
    return httpd_resp_send_chunk(req, NULL, 0); /* 结束响应 */
}

static const httpd_uri_t s_uri_song = {
    .uri      = "/song/*",
    .method   = HTTP_GET,
    .handler  = song_get_handler,
    .user_ctx = NULL,
};
static const httpd_uri_t s_uri_catchall = {
    .uri      = "/*",
    .method   = HTTP_GET,
    .handler  = send_index,
    .user_ctx = NULL,
};

/* ---------- 启停 ---------- */

static void start_dns(void)
{
    xTaskCreate(dns_server_task, "dns_srv", 3072, NULL, 5, &s_dns_task);
}

static esp_err_t start_hotspot(void)
{
    ensure_system_init();

    /* SSID：MusicBox + MAC 后两字节，免密（手机手动连即可） */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ssid, sizeof(s_ssid), "MusicBox-%02X%02X", mac[4], mac[5]);

    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi_init");

    wifi_config_t wcfg = { 0 };
    strncpy((char *)wcfg.ap.ssid, s_ssid, sizeof(wcfg.ap.ssid) - 1);
    wcfg.ap.channel       = AP_CHANNEL;
    wcfg.ap.max_connection = AP_MAX_CONN;
    wcfg.ap.authmode      = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wcfg), TAG, "set_config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi_start");

    /* HTTP 服务 */
    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.max_uri_handlers = 8;
    hcfg.stack_size = 6144; /* 默认 4096 不够用（snprintf 链 + 响应发送路径深） */
    hcfg.recv_wait_timeout = 10;
    hcfg.send_wait_timeout = 10;
    /* 默认匹配器是 strncmp 精确匹配，通配符 URI 必须显式指定匹配函数 */
    hcfg.uri_match_fn = httpd_uri_match_wildcard;
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &hcfg), TAG, "httpd_start");
    httpd_register_uri_handler(s_httpd, &s_uri_song);
    httpd_register_uri_handler(s_httpd, &s_uri_catchall);

    start_dns();

    s_enabled = true;
    s_last_http_ms = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "AP up: SSID=%s, IP=192.168.4.1, connect & open http://192.168.4.1", s_ssid);
    return ESP_OK;
}

static void stop_hotspot(void)
{
    s_enabled = false;
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    if (s_ap_netif) {
        esp_wifi_stop();
        esp_wifi_deinit();
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    ESP_LOGI(TAG, "AP down");
}

esp_err_t wifi_share_enable(bool enable)
{
    if (enable == s_enabled) {
        return ESP_OK;
    }
    if (enable) {
        return start_hotspot();
    }
    stop_hotspot();
    return ESP_OK;
}

bool wifi_share_is_enabled(void)
{
    return s_enabled;
}

/* 空闲自动关热点（由 ui_task 每 10ms 调 led_status_poll 时不适合；这里独立周期检查）。
 * 为避免开一个常驻任务，直接在 enable 后用 esp_timer 单次回调链——更简单：在发送端
 * touch_http 已更新时间，main 的 ui_task 可轮询 wifi_share_idle_ms() 并决定关闭。
 * 这里暴露一个查询接口供 ui_task 调用，避免本模块自起任务。 */
int64_t wifi_share_idle_ms(void)
{
    if (!s_enabled) {
        return 0;
    }
    return (esp_timer_get_time() / 1000) - s_last_http_ms;
}
