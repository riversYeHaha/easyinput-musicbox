#include <stdio.h>
#include <string.h>

#include "board_pins.h"
#include "buttons.h"
#include "encoder.h"
#include "led_status.h"
#include "player.h"
#include "power.h"
#include "proto.h"
#include "recorder.h"
#include "songbook.h"
#include "wifi_share.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "musicbox";

#define KEYTEST_DURATION_US (60 * 1000000LL)
#define VOLUME_DEFAULT      50
#define VOLUME_PER_DETENT   2

static int64_t s_keytest_until_us; /* >0 表示按键自检模式进行中 */
static int64_t s_rec_auto_stop_us; /* >0 表示 REPL `rec N` 自动停录截止时间 */
static uint8_t s_volume = VOLUME_DEFAULT;

/* ---------------- REPL 控制台（与 JSON 协议共用 USB CDC，'{' 开头的行走协议） ---------------- */

static int cmd_keytest(void)
{
    s_keytest_until_us = esp_timer_get_time() + KEYTEST_DURATION_US;
    printf("KEYTEST start (60s): press S1-S8 / rotate / press encoder.\n"
           "  Normal actions suspended; edges printed as S<n> DOWN/UP.\n"
           "  Note: keys are independent digital GPIO (active-low), not ADC matrix.\n");
    return 0;
}

static int cmd_ledtest(void)
{
    static const struct {
        led_mode_t mode;
        const char *name;
        int hold_ms;
    } demo[] = {
        { LED_MODE_RECORDING,  "recording (red on)",      2000 },
        { LED_MODE_GENERATING, "generating (marquee)",    3000 },
        { LED_MODE_DONE,       "done (green blink x3)",   2500 },
        { LED_MODE_PLAYING,    "playing (blue breathe)",  4000 },
    };
    printf("LEDTEST start: cycling all modes...\n");
    for (unsigned i = 0; i < sizeof(demo) / sizeof(demo[0]); i++) {
        printf("  mode: %s\n", demo[i].name);
        led_status_set_mode(demo[i].mode);
        vTaskDelay(pdMS_TO_TICKS(demo[i].hold_ms));
    }
    led_status_set_mode(LED_MODE_IDLE);
    printf("LEDTEST end (back to idle)\n");
    return 0;
}

static int cmd_rec(int sec)
{
    if (sec < 1) {
        sec = 1;
    } else if (sec > 30) {
        sec = 30; /* rec 分区容量上限 */
    }
    if (recorder_start() != ESP_OK) {
        printf("REC start failed (recording already? out of PSRAM?)\n");
        return -1;
    }
    s_rec_auto_stop_us = esp_timer_get_time() + (int64_t)sec * 1000000LL;
    printf("REC start, auto stop in %d s\n", sec);
    return 0;
}

static int cmd_recplay(void)
{
    if (recorder_is_playing()) {
        recorder_stop_play();
        printf("RECPLAY stop\n");
    } else if (recorder_play_last() == ESP_OK) {
        printf("RECPLAY start\n");
    } else {
        printf("RECPLAY failed (no recording / busy)\n");
    }
    return 0;
}

static int cmd_songs(void)
{
    printf("songs: %d in library, current=%d\n", songbook_count(), songbook_current());
    for (int i = 0; i < SONGBOOK_SLOTS; i++) {
        const char *t = songbook_get_title(i);
        if (t != NULL) {
            printf("  [%d] \"%s\"%s\n", i, t,
                   i == songbook_current() ? "  <- current" : "");
        } else {
            printf("  [%d] <empty>\n", i);
        }
    }
    return 0;
}

static int cmd_play(int index)
{
    esp_err_t err = player_play_index(index);
    printf("PLAY %d -> %s\n", index, esp_err_to_name(err));
    return err == ESP_OK ? 0 : -1;
}

static int cmd_stop(void)
{
    player_stop();
    recorder_stop_play();
    printf("STOP ok\n");
    return 0;
}

static int cmd_vol(int vol)
{
    if (vol < 0) {
        vol = 0;
    } else if (vol > 100) {
        vol = 100;
    }
    s_volume = (uint8_t)vol;
    player_set_volume(s_volume);
    proto_send_volume(s_volume);
    printf("VOL %u\n", s_volume);
    return 0;
}

/* 调试入口：进入 M3 收块模式（等价于上位机 song_begin），随后可推二进制帧验证 */
static int cmd_rxtest(int size)
{
    if (size <= 0) {
        size = 8192;
    }
    char line[96];
    snprintf(line, sizeof(line),
             "{\"cmd\":\"song_begin\",\"title\":\"rxtest\",\"size\":%d}", size);
    proto_process_line(line);
    printf("RXTEST size=%d: chunk mode on, push binary frames from host tool\n", size);
    return 0;
}

static int cmd_help(void)
{
    printf("commands:\n"
           "  keytest   60s key/encoder hardware self-test (prints GPIO edges, lights LEDs)\n"
           "  ledtest   cycle all LED modes (recording/generating/done/playing)\n"
           "  rec N     record N seconds (1-30) to rec partition, auto save\n"
           "  recplay   toggle replay of last recording\n"
           "  songs     list songbook slots\n"
           "  play N    play songbook slot N (0-%d)\n"
           "  stop      stop song playback and recording replay\n"
           "  vol N     set volume 0-100\n"
           "  rxtest N  enter song-chunk rx mode with declared size N (debug)\n"
           "  help      show this message\n"
           "lines starting with '{' go to the JSON protocol (ping/volume/gen_state/...)\n",
           SONGBOOK_SLOTS - 1);
    return 0;
}

static void repl_exec(char *line)
{
    int n;

    if (strcmp(line, "keytest") == 0) {
        cmd_keytest();
    } else if (strcmp(line, "ledtest") == 0) {
        cmd_ledtest();
    } else if (sscanf(line, "rec %d", &n) == 1) {
        cmd_rec(n);
    } else if (strcmp(line, "recplay") == 0) {
        cmd_recplay();
    } else if (strcmp(line, "songs") == 0) {
        cmd_songs();
    } else if (sscanf(line, "play %d", &n) == 1) {
        cmd_play(n);
    } else if (strcmp(line, "stop") == 0) {
        cmd_stop();
    } else if (sscanf(line, "vol %d", &n) == 1) {
        cmd_vol(n);
    } else if (sscanf(line, "rxtest %d", &n) == 1) {
        cmd_rxtest(n);
    } else if (strcmp(line, "rxtest") == 0) {
        cmd_rxtest(0);
    } else if (strcmp(line, "help") == 0) {
        cmd_help();
    } else {
        printf("unknown command: %s (try 'help')\n", line);
    }
}

/* 单字节路由：收块模式进协议状态机（M3 二进制帧），否则按行拼 REPL/JSON */
static void console_route_byte(uint8_t c, char *line, unsigned *len)
{
    if (proto_rx_binary()) {
        proto_rx_byte(c);
        return;
    }
    if (c == '\n' || c == '\r') {
        if (*len > 0) {
            line[*len] = '\0';
            if (line[0] == '{') {
                proto_process_line(line);
            } else {
                repl_exec(line);
            }
            *len = 0;
        }
        return;
    }
    if (*len < PROTO_LINE_MAX - 1) {
        line[(*len)++] = (char)c;
    } else {
        *len = 0; /* 超长行丢弃 */
    }
}

static void console_rx_task(void *arg)
{
    static char line[PROTO_LINE_MAX];
    unsigned len = 0;

    for (;;) {
        bool got = false;
        if (usb_serial_jtag_is_driver_installed()) {
            /* 裸读驱动环形缓冲（timeout=0）：必须绕开 stdin/vfs——sdkconfig 配了
             * LIBC_STDIN_LINE_ENDING_CR，vfs 读路径会把每个 0x0D 改写成 0x0A
             * （usb_serial_jtag_vfs.c:267），M3 二进制帧 payload 里的 0x0D 被篡改、
             * 帧 CRC 必失败；文本协议不受影响 */
            uint8_t buf[512];
            uint32_t n;
            while ((n = usb_serial_jtag_read_bytes(buf, sizeof(buf), 0)) > 0) {
                got = true;
                for (uint32_t i = 0; i < n; i++) {
                    console_route_byte(buf[i], line, &len);
                }
            }
        } else {
            /* 无驱动回退路径：stdin 直读 64B FIFO，仅服务文本协议 */
            int c;
            while ((c = getchar()) != EOF) {
                got = true;
                console_route_byte((uint8_t)c, line, &len);
            }
        }
        vTaskDelay(1); /* 抽干后/空转都睡 1 tick，防忙转饿死同核低优先级任务 */
    }
}

/* ---------------- 按键自检模式（挂起正常交互，打印电平沿+亮灯） ---------------- */

static void keytest_tick(void)
{
    static uint8_t s_prev;

    if (esp_timer_get_time() > s_keytest_until_us) {
        s_keytest_until_us = 0;
        s_prev = 0;
        led_status_set_mode(LED_MODE_IDLE);
        printf("KEYTEST end\n");
        return;
    }

    /* 排空正常事件，避免自检结束后积压触发 */
    for (uint8_t i = 0; i < BUTTONS_COUNT; i++) {
        (void)buttons_short_press(i);
        (void)buttons_long_start(i);
        (void)buttons_long_end(i);
    }

    /* 电平沿打印 */
    uint8_t cur = 0;
    for (uint8_t i = 0; i < BUTTONS_COUNT; i++) {
        if (buttons_level(i)) {
            cur |= (uint8_t)(1u << i);
        }
    }
    uint8_t ch = (uint8_t)(cur ^ s_prev);
    for (uint8_t i = 0; i < BUTTONS_COUNT; i++) {
        if (ch & (1u << i)) {
            printf("S%u %s\n", i + 1, (cur >> i) & 1 ? "DOWN" : "UP");
        }
    }
    s_prev = cur;

    int det = encoder_delta_detents();
    if (det != 0) {
        printf("ENC %+d detents\n", det);
    }
    if (encoder_short_press()) {
        printf("ENC press\n");
    }

    /* 灯反馈：有键按下时切录音红演示，松开回全灭（完整灯效用 ledtest） */
    led_status_set_mode(cur != 0 ? LED_MODE_RECORDING : LED_MODE_IDLE);
}

/* ---------------- UI 主循环：按键语义分发 ---------------- */

static void ui_task(void *arg)
{
    for (;;) {
        buttons_poll();
        encoder_poll();

        if (s_keytest_until_us != 0) {
            keytest_tick();
            led_status_poll();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* REPL `rec N` 自动停录（落盘擦写要数秒，阻塞 UI 属预期） */
        if (s_rec_auto_stop_us != 0 && esp_timer_get_time() >= s_rec_auto_stop_us) {
            s_rec_auto_stop_us = 0;
            esp_err_t err = recorder_stop();
            printf("REC auto stop -> %s\n", esp_err_to_name(err));
        }

        /* S1：随机生成纯音乐 */
        if (buttons_short_press(BTN_S1)) {
            ESP_LOGI(TAG, "S1: gen_req instrumental");
            proto_send_gen_req("instrumental");
        }
        /* S2：随机生成带歌词音乐 */
        if (buttons_short_press(BTN_S2)) {
            ESP_LOGI(TAG, "S2: gen_req song");
            proto_send_gen_req("song");
        }
        /* S3：长按录音（红灯），松开停止；recorder 内部先停播放防回授 */
        if (buttons_long_start(BTN_S3)) {
            ESP_LOGI(TAG, "S3 long: record start");
            if (recorder_start() != ESP_OK) {
                ESP_LOGW(TAG, "record start rejected");
            }
        }
        if (buttons_long_end(BTN_S3)) {
            ESP_LOGI(TAG, "S3 release: record stop");
            s_rec_auto_stop_us = 0; /* S3 与 rec N 任一先到都停 */
            if (recorder_stop() != ESP_OK) {
                ESP_LOGW(TAG, "record stop: nothing to save");
            }
        }
        /* S4：回放最近录音（再按停止） */
        if (buttons_short_press(BTN_S4)) {
            ESP_LOGI(TAG, "S4: %s", recorder_is_playing() ? "replay stop" : "replay last");
            if (recorder_is_playing()) {
                recorder_stop_play();
            } else {
                recorder_play_last(); /* 内部先停歌曲播放 */
            }
        }
        /* S5：哼歌成曲 */
        if (buttons_short_press(BTN_S5)) {
            ESP_LOGI(TAG, "S5: hum_req");
            proto_send_hum_req();
        }
        /* S6：提示词生成 */
        if (buttons_short_press(BTN_S6)) {
            ESP_LOGI(TAG, "S6: prompt_req");
            proto_send_prompt_req();
        }
        /* S7：切歌（到尾回卷；仅一首时重播） */
        if (buttons_short_press(BTN_S7)) {
            int idx = songbook_next();
            ESP_LOGI(TAG, "S7: next song -> index %d (count %d)", idx, songbook_count());
            if (idx >= 0) {
                player_play_index(idx);
            }
        }
        /* S8：Wi-Fi 热点开关 */
        if (buttons_short_press(BTN_S8)) {
            bool en = !wifi_share_is_enabled();
            ESP_LOGI(TAG, "S8: wifi_share %s", en ? "ON" : "OFF");
            wifi_share_enable(en);
        }

        /* 旋钮：旋转调音量（0-100），上报上位机 */
        int delta = encoder_delta_detents();
        if (delta != 0) {
            int vol = (int)s_volume + delta * VOLUME_PER_DETENT;
            if (vol < 0) {
                vol = 0;
            } else if (vol > 100) {
                vol = 100;
            }
            s_volume = (uint8_t)vol;
            ESP_LOGI(TAG, "volume %u", s_volume);
            player_set_volume(s_volume);
            proto_send_volume(s_volume);
        }
        /* 旋钮按压：播放/暂停切换，灯效由下方同步块跟随 */
        if (encoder_short_press()) {
            esp_err_t perr = player_pause_toggle();
            ESP_LOGI(TAG, "encoder press: play/pause -> %s", esp_err_to_name(perr));
        }

        /* LED 跟随音频链路状态；GENERATING/DONE 由协议事件驱动，不被覆盖 */
        led_mode_t lm = led_status_mode();
        if (recorder_is_recording()) {
            if (lm != LED_MODE_RECORDING) {
                led_status_set_mode(LED_MODE_RECORDING);
            }
        } else if (player_is_playing() || recorder_is_playing()) {
            if (lm != LED_MODE_PLAYING) {
                led_status_set_mode(LED_MODE_PLAYING);
            }
        } else if (lm == LED_MODE_RECORDING || lm == LED_MODE_PLAYING) {
            led_status_set_mode(LED_MODE_IDLE);
        }

        proto_poll(); /* M3 收块空闲超时中止 */

        /* 热点 10 分钟无访问自动关闭省电 */
        if (wifi_share_is_enabled() && wifi_share_idle_ms() > 10 * 60 * 1000) {
            ESP_LOGI(TAG, "wifi_share idle timeout, turning off AP");
            wifi_share_enable(false);
        }

        led_status_poll();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "EasyInput MusicBox | 板型 EasyInput V2.0 / v2 | PCB AI Keyboard V2.1 | ESP32-S3R8");

    if (power_up() != ESP_OK) {
        ESP_LOGE(TAG, "GPIO8 cold boot FAILED");
        return;
    }
    ESP_LOGI(TAG, "GPIO8 cold boot OK, shared rail ON (stable %d ms, 待实板测量确认)",
             POWER_STABLE_MS);

    buttons_init();
    encoder_init();
    led_status_init(); /* 失败时内部置空句柄，poll 静默跳过 */

    /* M2 音频三件套：录音（I2S0 麦→rec 分区）/ 播放（Helix→I2S1）/ 歌曲库（songs 分区） */
    recorder_init();
    player_init();
    songbook_init();

    proto_start(); /* USB CDC JSON 行协议 TX；上电自动发 hello */

    xTaskCreatePinnedToCore(ui_task, "ui", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(console_rx_task, "console", 4096, NULL, 6, NULL, 0);

    ESP_LOGI(TAG, "ready. REPL: 'keytest' / 'ledtest' / 'help'; JSON lines go to protocol");
}
