#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * 主机协议：USB CDC 单通道（控制台 USB Serial/JTAG），UTF-8 JSON 行 + M3 二进制分块帧。
 * JSON 一行一条、换行结束；非 '{' 开头的行忽略。
 *
 * 板 → 上位机（"cmd"/"ev"/"ack"/"err" 字段）：
 *   hello / gen_req / hum_req / prompt_req / state / song_list / song_stored(ev)
 *   {"ack":"song_begin"|seq|"song_end"}、{"err":code}（M3 写板应答）
 *   rec_meta(ev) / 录音帧 / rec_end(ev)：M4 录音回传（见下）
 * 上位机 → 板命令（实现方案 §3.4）：
 *   ping / volume / gen_state / song_list / song_begin / song_end / song_abort / rec_pull
 *
 * M4 rec_pull（板上最近录音 → 网页；方向与 M3 相反）：
 *   host 发 {"cmd":"rec_pull"} → 板 {"err":"rec_pull","msg":"no_recording"|"busy"|"io"}
 *   或直接回传：{"ev":"rec_meta","size":N,"rate":16000,"bits":16,"channels":1}
 *   → 连续若干帧（与 M3 完全同构：0xAA 0x55 | seq u16le | len u16le | payload |
 *     crc32 u32le(payload)，payload ≤4096，seq 从 0 递增）
 *   → {"ev":"rec_end","crc":<整段 PCM crc32>}。回传期间独占 TX（与 TX 任务互斥）。
 *   payload 为 16bit/mono 裸 PCM，按 size 截断，末帧可短。
 *
 * M3 收歌（数据量大，JSON 行不塞二进制）：
 *   song_begin{title,size} → {"ack":"song_begin","slot":i} 后进入收块模式；
 *   帧格式：0xAA 0x55 | seq u16le | len u16le | payload[len] | crc32 u32le(payload)
 *   停等 ACK：每帧回 {"ack":seq}；CRC/乱序回 {"nack":seq,"why":..} 保持会话，
 *   上位机停顿后整帧重发（同块连续 nack 超 SONG_NACK_MAX 才 err 中止）；
 *   song_end{crc}（整曲 CRC32）校验通过 → 落槽头 → {"ack":"song_end","ok":true}
 *   + {"ev":"song_stored","slot":i,"title":..}。
 *   CRC32 为标准实现（poly 0xEDB88320，初值 0xFFFFFFFF，末态取反），与网页端同一实现。
 *   收块模式下帧边界处收到 '{' 按内嵌 JSON 行处理（song_end/song_abort 等）。
 *   注：proto_start 会安装 USB Serial/JTAG 驱动（RX 环形缓冲 8KB）；
 *   缺省无驱动路径直读 64B 硬件 FIFO，二进制帧突发必丢字节。
 */

#define PROTO_LINE_MAX 320

/* 启动 TX 任务（RX 由 main 的控制台任务拼行后调 proto_process_line） */
void proto_start(void);

/* 行处理入口：非 '{' 开头的行忽略 */
void proto_process_line(const char *line);

/* ---- M3 收块模式（main 控制台据此把字节流转交 proto_rx_byte） ---- */
bool proto_rx_binary(void);
void proto_rx_byte(uint8_t b);

/* 收块空闲超时中止（UI 任务周期调用） */
void proto_poll(void);

/* ---- 板 → 上位机事件（UI 任务可调用，内部经队列转 TX 任务发送） ---- */

/* S1/S2：type = "instrumental" | "song" */
void proto_send_gen_req(const char *type);

/* S5 哼歌成曲 */
void proto_send_hum_req(void);

/* S6 提示词生成 */
void proto_send_prompt_req(void);

/* 音量同步（旋钮） */
void proto_send_volume(uint8_t vol);

/* 上线声明（hello），TX 任务启动时会自动发一次 */
void proto_send_hello(void);
