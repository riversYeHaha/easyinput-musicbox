#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * 歌曲库模块：
 * songs 分区（0xCF0000）等分 4 槽，每槽约 3.2MB：
 *   [槽头 96B（首 4KB 扇区内）][0x1000 起 MP3 数据区]
 * 槽头字段：magic/valid/title[48]/size/duration/序号 seq/CRC32。
 * 元数据开机扫描建 RAM 索引；当前曲目索引与全局 seq 持久化在 NVS（namespace "songbook"）。
 * 写新歌策略：优先空槽，无空槽覆盖序号最小（最旧）的一首。
 */

#define SONGBOOK_SLOTS 4
#define SONGBOOK_TITLE_MAX 48

/* 找分区并扫描槽头建索引；无有效歌曲时 count==0，不视为错误 */
esp_err_t songbook_init(void);

/* 库内歌曲数量（有效槽数） */
int songbook_count(void);

/* 当前曲目索引（-1 = 无） */
int songbook_current(void);

/* 取第 index 槽标题（无效槽返回 NULL） */
const char *songbook_get_title(int index);

/* 单槽 MP3 数据区容量（字节） */
uint32_t songbook_slot_size(void);

/* S7 切歌：跳到下一有效槽（到尾回卷；仅一首时重播），
 * 更新 NVS 当前索引，返回新索引（-1 = 空库） */
int songbook_next(void);

/* 流式读第 index 槽 MP3 数据（供播放器）；越界自动截短，返回实际字节数 */
esp_err_t songbook_read(int index, uint32_t offset, void *buf, size_t len, size_t *out_len);

/* 写歌三段式（M3 上位机写板用；一次只允许一个写会话） */
esp_err_t songbook_write_begin(const char *title, uint32_t size);
esp_err_t songbook_write_chunk(const void *buf, size_t len);
esp_err_t songbook_write_end(uint32_t duration_sec); /* 失败/中止传 duration=0 且 size 未写满则作废该槽 */
void songbook_write_abort(void);

/* 当前写会话的槽位（无会话返回 -1），M3 协议回 ACK/上报用 */
int songbook_write_slot(void);

/* 第 index 槽 MP3 数据字节数（无效槽返回 0），M3 song_list 用 */
uint32_t songbook_get_size(int index);

/* 第 index 槽时长（秒，0=未知），M5 下载页用 */
uint32_t songbook_get_duration(int index);

/* 格式化：擦全部槽头，清空索引与 NVS 当前索引 */
esp_err_t songbook_format(void);
