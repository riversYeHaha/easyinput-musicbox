#include "proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "led_status.h"
#include "player.h"
#include "recorder.h"
#include "songbook.h"

static const char *TAG = "proto";

/* 发送队列：malloc 的字符串副本，TX 任务打印后释放 */
static QueueHandle_t s_tx_q;

/* TX 互斥：TX 任务的 JSON 行与 rec_pull 的直接二进制回传串行化，防字节交错 */
static SemaphoreHandle_t s_tx_mutex;

/* ---- M3 二进制分块帧（见 proto.h 协议说明） ---- */
#define FRAME_SYNC0             0xAAu
#define FRAME_SYNC1             0x55u
#define SONG_CHUNK_MAX          4096u
#define SONG_RX_IDLE_TIMEOUT_MS 15000 /* 收块空闲超时：中止会话 */
#define SONG_NACK_MAX           3     /* 同一块连续 nack 超过此数才中止会话 */

/* USB CDC RX 环形缓冲：必须 ≥ 一整帧（4106B）+ 余量。
 * 不装驱动时 vfs 走无驱动路径直读 64B 硬件 FIFO，4KB 帧突发必丢字节（M3 首块 CRC 失败根因） */
#define CDC_RX_BUF_SIZE         8192
#define CDC_TX_BUF_SIZE         8192

/* ---- CRC32：标准实现（poly 0xEDB88320，初值 0xFFFFFFFF，末态取反），
 *     与网页端 main.js 同一实现；帧 CRC 与整曲 CRC 均用它 ---- */
static uint32_t s_crc_tab[256];

static void crc_tab_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        s_crc_tab[i] = c;
    }
}

/* 增量接口：crc 传入/传出均为 raw（未取反） */
static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, size_t len)
{
    while (len--) {
        crc = s_crc_tab[(crc ^ *buf++) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

/* 单块完整 CRC（首尾包初值/取反） */
static uint32_t crc32_oneshot(const uint8_t *buf, size_t len)
{
    return crc32_update(0xFFFFFFFFu, buf, len) ^ 0xFFFFFFFFu;
}

/* ---- TX ---- */

static void proto_tx_line(char *line)
{
    if (!s_tx_q || !line) {
        free(line);
        return;
    }
    if (xQueueSend(s_tx_q, &line, 0) != pdTRUE) {
        free(line); /* 满则丢 */
    }
}

/* 构造 JSON 并入队（cJSON 会分配内存，调用方需在任务上下文） */
static void send_json(cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (s) {
        proto_tx_line(s);
    }
}

static void send_err(const char *code, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "err", code);
    if (msg) {
        cJSON_AddStringToObject(o, "msg", msg);
    }
    send_json(o);
}

void proto_send_hello(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "cmd", "hello");
    cJSON_AddNumberToObject(o, "v", 1);
    cJSON_AddStringToObject(o, "name", "EasyInput MusicBox");
    cJSON *caps = cJSON_AddArrayToObject(o, "caps");
    cJSON_AddItemToArray(caps, cJSON_CreateString("rec"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("mp3"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("wifi_share"));
    cJSON_AddNumberToObject(o, "songs", songbook_count());
    send_json(o);
}

void proto_send_gen_req(const char *type)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "cmd", "gen_req");
    cJSON_AddStringToObject(o, "type", type);
    send_json(o);
}

void proto_send_hum_req(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "cmd", "hum_req");
    send_json(o);
}

void proto_send_prompt_req(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "cmd", "prompt_req");
    send_json(o);
}

void proto_send_volume(uint8_t vol)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "cmd", "state");
    cJSON_AddNumberToObject(o, "vol", vol);
    send_json(o);
}

static void proto_tx_task(void *arg)
{
    proto_send_hello(); /* 上电主动声明能力 */

    for (;;) {
        char *line = NULL;
        if (xQueueReceive(s_tx_q, &line, portMAX_DELAY) == pdTRUE && line) {
            xSemaphoreTake(s_tx_mutex, portMAX_DELAY); /* 与 rec_pull 回传互斥 */
            printf("%s\n", line); /* USB CDC（控制台 USB Serial/JTAG 即 stdout） */
            xSemaphoreGive(s_tx_mutex);
            free(line);
        }
    }
}

/* ---- M3 收块会话状态 ---- */

typedef enum {
    RXS_IDLE = 0, /* 帧边界：等 0xAA（帧头）或 '{'（内嵌 JSON 行） */
    RXS_SYNC1,    /* 已收 0xAA，等 0x55 */
    RXS_HDR,      /* 收 seq/len 共 4 字节 */
    RXS_PAYLOAD,  /* 收 len 字节数据 */
    RXS_CRC,      /* 收 4 字节 CRC */
    RXS_LINE,     /* 收块模式内嵌 JSON 行（song_end/song_abort 等） */
} rx_state_t;

static bool      s_rx_binary;              /* 收块模式开关（song_begin 成功后置位） */
static rx_state_t s_rx_st = RXS_IDLE;
static uint8_t   s_hdr[4];
static uint16_t  s_rx_seq, s_rx_len;       /* 当前帧序号/数据长 */
static uint32_t  s_rx_frm_crc;             /* 当前帧携带的 CRC */
static unsigned  s_rx_pos;                 /* 各阶段通用下标 */
static uint8_t   s_chunk[SONG_CHUNK_MAX];  /* 当前帧数据 */
static char      s_rx_line[PROTO_LINE_MAX];
static uint16_t  s_expect_seq;             /* 停等：下一个期望序号 */
static uint32_t  s_song_crc;               /* 整曲 CRC raw（比较时取反） */
static int64_t   s_rx_last_ms;             /* 最近收字节时刻（空闲超时用） */
static unsigned  s_nack_cnt;               /* 当前期望块的连续 nack 次数 */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void rx_session_close(void)
{
    s_rx_binary = false;
    s_rx_st = RXS_IDLE;
    s_rx_pos = 0;
}

/* 写板灯效：录音中不抢灯（ui 循环也会让 RECORDING 优先，这里先避让） */
static void led_write_mode(led_mode_t m)
{
    if (led_status_mode() != LED_MODE_RECORDING) {
        led_status_set_mode(m);
    }
}

/* 会话失败统一收尾：作废槽、退出收块、灯复位 */
static void rx_session_fail(void)
{
    songbook_write_abort();
    rx_session_close();
    led_write_mode(LED_MODE_IDLE);
}

static void send_chunk_ack(uint16_t seq)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "ack", seq);
    send_json(o);
}

/* 丢弃板端已缓冲的输入字节：清空残留（被错位消耗的半帧等）防止状态机错误重同步。
 * 必须非阻塞——直接用 USJ 驱动 API（timeout=0）读到返回 0 为止；
 * 不能用 getchar()：缓冲空时 vfs 读会阻塞，控制台任务卡死、nack 发不出（实板教训）。
 * 上位机收到 nack 后停 50ms 才整帧重发，此处在微秒级完成，不会吞到重发帧 */
static void rx_drain_input(void)
{
    if (!usb_serial_jtag_is_driver_installed()) {
        return; /* 无驱动回退路径只服务文本协议，无从排空 */
    }
    uint8_t tmp[128];
    while (usb_serial_jtag_read_bytes(tmp, sizeof(tmp), 0) > 0) {
    }
}

/* 拒收当前帧但保持会话：上位机应在重试上限内重发该块 */
static void send_chunk_nack(uint16_t seq, const char *why)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "nack", seq);
    cJSON_AddStringToObject(o, "why", why);
    cJSON_AddNumberToObject(o, "expect", s_expect_seq);
    send_json(o);
}

/* nack 收尾：计数+清残留+回 nack；超过上限才中止会话 */
static void nack_or_fail(uint16_t seq, const char *why)
{
    s_nack_cnt++;
    ESP_LOGW(TAG, "chunk %u nack(%s) #%u, expect %u", seq, why, s_nack_cnt, s_expect_seq);
    if (s_nack_cnt > SONG_NACK_MAX) {
        send_err(why, "too many nacks");
        rx_session_fail();
        return;
    }
    /* 先发 nack 再排空：排空绝不能排在 nack 前（排空已改非阻塞，但顺序上仍
     * 保证 nack 最先出队，上位机尽早进入重发） */
    send_chunk_nack(seq, why);
    rx_drain_input();
}

/* 完整收好一帧数据（头部/CRC 已剥） */
static void handle_chunk_frame(void)
{
    if (crc32_oneshot(s_chunk, s_rx_len) != s_rx_frm_crc) {
        nack_or_fail(s_rx_seq, "crc");
        return;
    }
    /* 重发块（上位机 ACK 超时重试）：数据已落盘，直接再 ACK */
    if (s_expect_seq > 0 && s_rx_seq == (uint16_t)(s_expect_seq - 1)) {
        send_chunk_ack(s_rx_seq);
        return;
    }
    if (s_rx_seq != s_expect_seq) {
        nack_or_fail(s_rx_seq, "seq");
        return;
    }
    esp_err_t err = songbook_write_chunk(s_chunk, s_rx_len);
    if (err != ESP_OK) {
        /* flash 写失败非传输问题，直接中止 */
        ESP_LOGW(TAG, "chunk %u write failed: %s", s_rx_seq, esp_err_to_name(err));
        send_err("write", esp_err_to_name(err));
        rx_session_fail();
        return;
    }
    s_song_crc = crc32_update(s_song_crc, s_chunk, s_rx_len);
    send_chunk_ack(s_rx_seq);
    s_expect_seq++;
    s_nack_cnt = 0;
}

static void handle_song_begin(const cJSON *root)
{
    if (s_rx_binary) {
        /* 上位机重试/重开：先丢弃旧会话 */
        ESP_LOGW(TAG, "song_begin while active, drop old session");
        rx_session_fail();
    }
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(root, "title");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (!cJSON_IsNumber(size) || size->valuedouble <= 0 ||
        (uint32_t)size->valuedouble > songbook_slot_size()) {
        send_err("song_begin", "bad size");
        return;
    }
    const char *t = cJSON_IsString(title) ? title->valuestring : "untitled";
    /* 标题按槽头 48 字节截断由 songbook_write_begin 内部完成 */
    esp_err_t err = songbook_write_begin(t, (uint32_t)size->valuedouble);
    if (err != ESP_OK) {
        send_err("song_begin", esp_err_to_name(err));
        return;
    }
    s_rx_binary = true;
    s_rx_st = RXS_IDLE;
    s_rx_pos = 0;
    s_expect_seq = 0;
    s_song_crc = 0xFFFFFFFFu;
    s_nack_cnt = 0;
    s_rx_last_ms = now_ms();
    led_write_mode(LED_MODE_GENERATING); /* 写块期间复用跑马灯 */

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ack", "song_begin");
    cJSON_AddNumberToObject(o, "slot", songbook_write_slot());
    cJSON_AddNumberToObject(o, "max_chunk", SONG_CHUNK_MAX);
    send_json(o);
}

static void handle_song_end(const cJSON *root)
{
    if (!s_rx_binary) {
        send_err("song_end", "no session");
        return;
    }
    const cJSON *crc = cJSON_GetObjectItemCaseSensitive(root, "crc");
    uint32_t final_crc = s_song_crc ^ 0xFFFFFFFFu;
    if (!cJSON_IsNumber(crc) || (uint32_t)crc->valuedouble != final_crc) {
        ESP_LOGW(TAG, "song_end crc: host 0x%lx != board 0x%lx",
                 cJSON_IsNumber(crc) ? (unsigned long)(uint32_t)crc->valuedouble : 0ul,
                 (unsigned long)final_crc);
        send_err("song_end", "crc mismatch");
        rx_session_fail();
        return;
    }
    int slot = songbook_write_slot();
    esp_err_t err = songbook_write_end(0); /* 时长未知，记 0 */
    rx_session_close();
    if (err != ESP_OK) {
        led_write_mode(LED_MODE_IDLE);
        send_err("song_end", esp_err_to_name(err));
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ack", "song_end");
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "slot", slot);
    send_json(o);

    /* 落盘成功上报 */
    const char *t = songbook_get_title(slot);
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "ev", "song_stored");
    cJSON_AddNumberToObject(e, "slot", slot);
    cJSON_AddStringToObject(e, "title", t ? t : "");
    send_json(e);
    led_write_mode(LED_MODE_DONE); /* 绿闪 3 次 */
}

static void handle_song_abort(void)
{
    if (!s_rx_binary) {
        return; /* 无会话，静默 */
    }
    ESP_LOGW(TAG, "song_abort from host");
    rx_session_fail();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ack", "song_abort");
    send_json(o);
}

static void handle_song_list(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "cmd", "song_list");
    cJSON_AddNumberToObject(o, "slots", SONGBOOK_SLOTS);
    cJSON_AddNumberToObject(o, "count", songbook_count());
    cJSON_AddNumberToObject(o, "current", songbook_current());
    cJSON *arr = cJSON_AddArrayToObject(o, "songs");
    for (int i = 0; i < SONGBOOK_SLOTS; i++) {
        const char *t = songbook_get_title(i);
        if (t == NULL) {
            continue;
        }
        cJSON *s = cJSON_CreateObject();
        cJSON_AddNumberToObject(s, "slot", i);
        cJSON_AddStringToObject(s, "title", t);
        cJSON_AddNumberToObject(s, "size", songbook_get_size(i));
        cJSON_AddItemToArray(arr, s);
    }
    send_json(o);
}

/* ---- M4 rec_pull：板上录音回传（方向与 M3 相反，板 → 网页） ---- */

/* 绕过 TX 队列直接阻塞写 USJ，直到整段写完。
 * 关键约束：usb_serial_jtag_write_bytes 内部用 xRingbufferSend(BYTEBUF)，
 * 当 size > tx_buffer_size 时立即返回 0（不等待、不写入）。因此单次写入
 * 必须 ≤ CDC_TX_BUF_SIZE，此处按此上限分块，配合 stalls 超时检测主机读取。 */
static esp_err_t usj_write_all(const uint8_t *buf, size_t len)
{
    size_t off = 0;
    int stalls = 0;
    while (off < len) {
        size_t want = len - off;
        if (want > CDC_TX_BUF_SIZE) {
            want = CDC_TX_BUF_SIZE;
        }
        int n = usb_serial_jtag_write_bytes((const void *)(buf + off), want,
                                            pdMS_TO_TICKS(1000));
        if (n > 0) {
            off += (size_t)n;
            stalls = 0;
        } else if (++stalls > 4) {
            ESP_LOGW(TAG, "usj tx stalled @%u/%u", (unsigned)off, (unsigned)len);
            return ESP_ERR_TIMEOUT; /* 主机长时间不取，放弃（上位机整段 CRC 兜底） */
        }
    }
    return ESP_OK;
}

/* 直接写一行 JSON（自带换行）；调用方需持有 s_tx_mutex */
static esp_err_t usj_write_json(cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (s == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = usj_write_all((const uint8_t *)s, strlen(s));
    if (err == ESP_OK) {
        err = usj_write_all((const uint8_t *)"\n", 1);
    }
    free(s);
    return err;
}

static void handle_rec_pull(void)
{
    if (s_rx_binary) {
        send_err("rec_pull", "busy"); /* 正在写歌会话中，别打断 */
        return;
    }
    if (recorder_is_recording()) {
        send_err("rec_pull", "busy"); /* 录音进行中，分区数据不稳定 */
        return;
    }
    uint32_t frames = 0, rate = 0;
    if (recorder_last_meta(&frames, &rate) != ESP_OK) {
        send_err("rec_pull", "no_recording");
        return;
    }
    uint32_t total = frames * sizeof(int16_t);
    ESP_LOGI(TAG, "rec_pull: %lu frames (%lu B) @%lu Hz", (unsigned long)frames,
             (unsigned long)total, (unsigned long)rate);

    /* 整段回传持有 TX 互斥：与 TX 任务的 JSON 行严格串行，帧内不会被插入字节 */
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "ev", "rec_meta");
    cJSON_AddNumberToObject(meta, "size", (double)total);
    cJSON_AddNumberToObject(meta, "rate", (double)rate);
    cJSON_AddNumberToObject(meta, "bits", 16);
    cJSON_AddNumberToObject(meta, "channels", 1);
    if (usj_write_json(meta) != ESP_OK) {
        xSemaphoreGive(s_tx_mutex);
        return;
    }

    uint32_t seq = 0, off = 0;
    uint32_t whole_crc = 0xFFFFFFFFu;
    bool io_err = false;
    while (off < total) {
        uint32_t n = total - off;
        if (n > SONG_CHUNK_MAX) {
            n = SONG_CHUNK_MAX;
        }
        uint32_t got = 0;
        if (recorder_read(off, s_chunk, n, &got) != ESP_OK || got != n) {
            io_err = true;
            break;
        }
        /* 与 M3 完全相同的帧：0xAA 0x55 | seq u16le | len u16le | payload | crc32 u32le */
        uint8_t hdr[6] = { FRAME_SYNC0, FRAME_SYNC1, (uint8_t)(seq & 0xFF),
                           (uint8_t)(seq >> 8), (uint8_t)(n & 0xFF), (uint8_t)(n >> 8) };
        uint32_t fc = crc32_oneshot(s_chunk, n);
        uint8_t crcb[4] = { (uint8_t)fc, (uint8_t)(fc >> 8), (uint8_t)(fc >> 16),
                            (uint8_t)(fc >> 24) };
        if (usj_write_all(hdr, sizeof(hdr)) != ESP_OK
            || usj_write_all(s_chunk, n) != ESP_OK
            || usj_write_all(crcb, sizeof(crcb)) != ESP_OK) {
            io_err = true;
            break;
        }
        whole_crc = crc32_update(whole_crc, s_chunk, n);
        off += n;
        seq++;
        /* 注意：切勿在此帧边界向流里插入 JSON 心跳（如 rec_progress）。
         * 网页帧解析器按长度推进、整段收完才切回 text 模式，
         * 帧流中混入的非 0xAA 字节会被误当作帧头/长度，导致 CRC 校验失败。
         * 进度可观测性由网页侧 onRecFrame 每 16 帧打印一次承担。 */
    }

    if (io_err) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "err", "rec_pull");
        cJSON_AddStringToObject(e, "msg", "io");
        usj_write_json(e);
        xSemaphoreGive(s_tx_mutex);
        ESP_LOGW(TAG, "rec_pull aborted @%lu/%lu", (unsigned long)off, (unsigned long)total);
        return;
    }

    cJSON *end = cJSON_CreateObject();
    cJSON_AddStringToObject(end, "ev", "rec_end");
    cJSON_AddNumberToObject(end, "crc", (double)(whole_crc ^ 0xFFFFFFFFu));
    usj_write_json(end);
    xSemaphoreGive(s_tx_mutex);
    ESP_LOGI(TAG, "rec_pull done: %lu frame(s), %lu B", (unsigned long)seq,
             (unsigned long)off);
}

/* ---- RX ---- */

static const cJSON *cmd_of(const cJSON *root)
{
    /* 兼容 "cmd" 与 "t" 两种字段名 */
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(c)) {
        c = cJSON_GetObjectItemCaseSensitive(root, "t");
    }
    return c;
}

static void handle_line(const char *line)
{
    cJSON *root = cJSON_Parse(line);
    if (!root) {
        return;
    }
    const cJSON *cmd = cmd_of(root);
    if (!cJSON_IsString(cmd)) {
        goto done;
    }
    const char *c = cmd->valuestring;

    if (strcmp(c, "ping") == 0) {
        proto_send_hello();
    } else if (strcmp(c, "volume") == 0) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "v");
        if (cJSON_IsNumber(v)) {
            player_set_volume((uint8_t)v->valueint);
        }
    } else if (strcmp(c, "gen_state") == 0) {
        /* 生成中 → 跑马灯；完成 → 绿闪 3 次 */
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "v");
        if (cJSON_IsString(v) && strcmp(v->valuestring, "start") == 0) {
            led_status_set_mode(LED_MODE_GENERATING);
        } else if (cJSON_IsString(v) && strcmp(v->valuestring, "done") == 0) {
            led_status_set_mode(LED_MODE_DONE);
        }
    } else if (strcmp(c, "song_begin") == 0) {
        handle_song_begin(root);
    } else if (strcmp(c, "song_end") == 0) {
        handle_song_end(root);
    } else if (strcmp(c, "song_abort") == 0) {
        handle_song_abort();
    } else if (strcmp(c, "song_list") == 0) {
        handle_song_list();
    } else if (strcmp(c, "rec_pull") == 0) {
        handle_rec_pull();
    }

done:
    cJSON_Delete(root);
}

void proto_process_line(const char *line)
{
    if (line[0] == '{') {
        handle_line(line);
    }
}

/* ---- M3 收块模式字节入口：环形字节流上的帧/行状态机 ---- */

bool proto_rx_binary(void)
{
    return s_rx_binary;
}

void proto_rx_byte(uint8_t b)
{
    s_rx_last_ms = now_ms();

    switch (s_rx_st) {
    case RXS_IDLE:
        if (b == FRAME_SYNC0) {
            s_rx_st = RXS_SYNC1;
        } else if (b == '{') {
            s_rx_line[0] = '{';
            s_rx_pos = 1;
            s_rx_st = RXS_LINE;
        }
        /* 其余字节：噪声/杂散，丢弃 */
        break;
    case RXS_SYNC1:
        if (b == FRAME_SYNC1) {
            s_rx_pos = 0;
            s_rx_st = RXS_HDR;
        } else if (b == '{') {
            s_rx_line[0] = '{';
            s_rx_pos = 1;
            s_rx_st = RXS_LINE;
        } else if (b != FRAME_SYNC0) {
            s_rx_st = RXS_IDLE; /* 0xAA 后非 0x55：回到帧边界（连续 0xAA 则继续等） */
        }
        break;
    case RXS_HDR:
        s_hdr[s_rx_pos++] = b;
        if (s_rx_pos == sizeof(s_hdr)) {
            s_rx_seq = (uint16_t)(s_hdr[0] | ((uint16_t)s_hdr[1] << 8));
            s_rx_len = (uint16_t)(s_hdr[2] | ((uint16_t)s_hdr[3] << 8));
            if (s_rx_len == 0 || s_rx_len > SONG_CHUNK_MAX) {
                ESP_LOGW(TAG, "bad frame len %u", s_rx_len);
                send_err("frame", "bad len");
                rx_session_fail();
                break;
            }
            s_rx_pos = 0;
            s_rx_st = RXS_PAYLOAD;
        }
        break;
    case RXS_PAYLOAD:
        s_chunk[s_rx_pos++] = b;
        if (s_rx_pos >= s_rx_len) {
            s_rx_pos = 0;
            s_rx_frm_crc = 0;
            s_rx_st = RXS_CRC;
        }
        break;
    case RXS_CRC:
        s_rx_frm_crc |= (uint32_t)b << (8 * s_rx_pos);
        s_rx_pos++;
        if (s_rx_pos == 4) {
            s_rx_pos = 0;
            s_rx_st = RXS_IDLE;
            handle_chunk_frame(); /* 校验/落盘/ACK 或中止 */
        }
        break;
    case RXS_LINE:
        if (b == '\n' || b == '\r') {
            if (s_rx_pos > 0) {
                s_rx_line[s_rx_pos] = '\0';
                s_rx_pos = 0;
                s_rx_st = RXS_IDLE;
                handle_line(s_rx_line); /* 内嵌 JSON 行（song_end/song_abort 等） */
            } else {
                s_rx_st = RXS_IDLE;
            }
        } else if (s_rx_pos < PROTO_LINE_MAX - 1) {
            s_rx_line[s_rx_pos++] = (char)b;
        } else {
            s_rx_pos = 0;
            s_rx_st = RXS_IDLE; /* 超长行丢弃 */
        }
        break;
    }
}

void proto_poll(void)
{
    if (s_rx_binary && (now_ms() - s_rx_last_ms) > SONG_RX_IDLE_TIMEOUT_MS) {
        ESP_LOGW(TAG, "song rx idle %d ms, abort session", SONG_RX_IDLE_TIMEOUT_MS);
        send_err("rx_timeout", "idle too long");
        rx_session_fail();
    }
}

/* 安装 USB Serial/JTAG 驱动并把 console vfs 切到驱动路径：
 * 缺省无驱动路径直读 64B 硬件 FIFO，M3 二进制帧突发必溢出丢字节 */
static void cdc_driver_init(void)
{
    if (usb_serial_jtag_is_driver_installed()) {
        return;
    }
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = CDC_TX_BUF_SIZE,
        .rx_buffer_size = CDC_RX_BUF_SIZE,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        /* 文本协议仍可工作（无驱动路径），仅 M3 大帧受影响 */
        ESP_LOGE(TAG, "usj driver install failed: %s", esp_err_to_name(err));
        return;
    }
    usb_serial_jtag_vfs_use_driver();
}

void proto_start(void)
{
    cdc_driver_init();
    crc_tab_init();
    s_tx_mutex = xSemaphoreCreateMutex();
    s_tx_q = xQueueCreate(16, sizeof(char *));
    xTaskCreatePinnedToCore(proto_tx_task, "proto_tx", 4096, NULL, 6, NULL, 0);
}
