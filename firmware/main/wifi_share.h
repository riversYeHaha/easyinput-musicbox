#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Wi-Fi 传歌模块（M5）：
 * S8 开/关 SoftAP（SSID MusicBox-XXXX，免密）+ esp_http_server：
 *   GET /         → 歌曲列表页（Captive Portal 任意域名也回此页）
 *   GET /song/{n}.mp3 → 流式下载第 n 槽 MP3，支持 Range
 * 另起一个极简 DNS 服务器把所有域名解析到 192.168.4.1，手机连热点自动弹出"登录"页。
 * 热点开启期间若 10 分钟无 HTTP 访问，调用方应依据 wifi_share_idle_ms() 主动关闭省电。
 */

/* 开/关热点与 HTTP 服务 */
esp_err_t wifi_share_enable(bool enable);

bool wifi_share_is_enabled(void);

/* 距最近一次 HTTP 请求的毫秒数（热点关闭时返回 0），供 ui_task 做空闲自动关热点 */
int64_t wifi_share_idle_ms(void);
