# EasyInput 奇妙音乐盒（musicbox）实现方案

> 硬件基线：EasyInput V2.0（ESP32-S3R8 + 8MB PSRAM + 16MB Flash），硬件事实以《EasyInput-V2.0-开发基线文档》为准
> 状态：方案已确认，待动工

---

## 1. 需求概述

软硬件结合的"奇妙音乐盒"，整体形似百宝袋，可播放各种各样的音乐。

### 按键规划

| 按键 | 功能 | 依赖 |
| --- | --- | --- |
| S1 | 随机生成纯音乐 | 需联网，配套软件 |
| S2 | 随机生成带歌词音乐 | 需联网，配套软件 |
| S3 | 长按录音（松开停止） | 板端独立 |
| S4 | 播放录音 | 板端独立 |
| S5 | 哼歌成曲（仿写/扩写，使用录音内容） | 需联网，配套软件 |
| S6 | 提示词生成纯音乐（录音转文字作为提示词） | 需联网，配套软件 |
| S7 | 切歌：播放中切到下一首（到尾回卷；仅一首时重播） | 板端独立 |
| S8 | Wi-Fi 热点开关：开热点后手机连接，浏览器访问板内网页下载歌曲 | 板端 + 手机浏览器 |
| 旋钮 | 旋转控制音量，按压播放/暂停 | 板端独立 |

### 灯光语义（5 颗 WS2812）

- **红灯常亮** = 录音中
- **跑马灯旋转** = 生成中
- **闪烁（绿闪 3 次）** = 生成完毕
- 播放中呼吸灯（项目自定义，可后调）

### 核心约束

1. 生成的歌曲写入板载 Flash，**离线可随时播放**；AI 生成等其余功能依赖配套软件对接音乐生成模型，生成后再写入开发板。
2. Flash 16MB。经评估采用 MP3 格式后容量比原预估宽松（见 §3.2），不再局限于一首歌。

---

## 2. 总体架构

的分层架构：

```
┌─ 固件 (ESP-IDF, EasyInput V2.0) ─────────┐   USB CDC（JSON 行协议）  ┌─ 配套网页·电脑端 (Vite) ──┐
│ 按键/旋钮/LED 灯效/录音/MP3 播放/歌曲库    │ ◄═══════════════════► │ 阶跃 API 适配层/音频转码      │
│ Wi-Fi SoftAP + HTTP 服务器（歌曲下载页）   │ ◄── Wi-Fi 热点，HTTP ── │ 手机浏览器（仅取歌，免安装）  │
└────────────────────────────────────────────┘                         └──────────────────────────────┘
```

- **固件**：ESP-IDF 新工程，位于 `easyinput-musicbox/firmware/`，代码骨架从 hajimibeatbox 搬运并重新定义按键/协议语义（属本项目私有合同，不跨工程继承默认值）。
- **配套软件**：Vite 网页，**仅 Web Serial（USB）通道**，负责 AI 生成与写板，免安装。
- **职责划分**：电脑 + USB = 生成与写入；手机 + Wi-Fi 热点 = 浏览/下载歌曲（手机连热点期间无外网，不承担 AI 生成）。

---

## 3. 固件设计

### 3.1 模块划分（`firmware/main/`）

| 模块 | 职责 | 来源 |
| --- | --- | --- |
| `board_pins.h` | 引脚表（S1=GPIO2 … S8=GPIO48，编码器 17/16/18，PWR_EN=GPIO8，LED=GPIO12，MIC=9/10/11，SPK=14/13/15） | 搬运 + 按基线文档 §2 核对 |
| `power.c` | GPIO8 共享电源冷启动顺序（基线文档 §4，稳定时间自行实测定义） | 搬运 |
| `buttons.c` | 8 路独立低有效输入 + 消抖 + S3 长按检测 | 搬运 + 扩展长按 |
| `encoder.c` | 正交解码（音量）+ 短按（播放/暂停） | 搬运 |
| `led_status.c` | WS2812 帧管理 + 灯效状态机（录音红/跑马灯/完成闪烁/播放呼吸） | 搬运 + 新灯效 |
| `recorder.c` | I2S 麦 → PCM → `rec` 录音分区；S4 回放 | 参照 repeater 录音链路 |
| `player.c` | MP3 流 → Helix 解码 → I2S MAX98357A | 新增 |
| `songbook.c` | 歌曲库存取：环形槽位 + 元数据（NVS） | 新增 |
| `proto.c` | JSON 行协议收发（USB CDC） | 搬运 + 扩展命令 |
| `wifi_share.c` | Wi-Fi SoftAP + `esp_http_server`：歌曲列表页 + MP3 下载接口，S8 开关 | 新增 |
| `main.c` | 按键语义分发 + 主状态机 + REPL 自检（keytest 模式） | 重写 |

### 3.2 存储与分区

16MB Flash 规划（项目私有合同）：

| 分区 | 大小 | 用途 |
| --- | --- | --- |
| nvs / phy_init | 20KB | 系统 + 歌曲元数据（标题/时长/来源/槽位） |
| factory app | 2MB | 固件（含 Helix 解码器） |
| `rec` 数据区 | ~1MB | 最近一段录音，16kHz/16bit/mono PCM，约 30 秒 |
| `songs` 数据区 | ~12MB | 生成歌曲，MP3 格式 |

**容量核算（3 分钟歌曲）**：

| 格式 | 单首大小 | 可存数量 | 说明 |
| --- | --- | --- | --- |
| 裸 PCM 32k/16bit/mono | ~11.5MB | 1 | 放弃 |
| **MP3 128kbps（选定）** | ~2.9MB | **3~4 首** | Helix 解码，ESP-IDF 有成熟组件（esp_audio_codec / helix-mp3） |
| IMA-ADPCM | ~2.9MB | 3~4 | 备选，解码极简但音质一般 |

- 歌曲区满时默认行为：**自动覆盖最旧一首**（待用户最终确认，可改为提示上位机手动管理）。
- 播放链路离线可用，不依赖 USB/BLE 连接。

### 3.3 录音

- 参数：16kHz / 16bit / mono PCM（复用 repeater 验证过的链路），最长约 30 秒（`rec` 区大小可调）。
- S3 长按开始（红灯亮），松开停止并落 `rec` 分区；S4 短按回放最近录音。
- 录音可被上位机通过 `rec_pull` 命令拉取（供 S5/S6 使用）。

### 3.4 协议（JSON 行，USB CDC 单通道）

上位机 → 板：

| 命令 | 说明 |
| --- | --- |
| `song_begin {title, size, crc}` | 开始写入新歌，分配槽位 |
| 二进制分块帧 | 音频数据分块传输（带序号与校验） |
| `song_end {crc}` | 结束并校验，更新元数据 |
| `rec_pull` | 请求上传当前录音 |
| `gen_state {start\|done}` | 驱动灯效：跑马灯 / 完成闪烁 |
| `volume {0-100}` | 音量同步 |
| `ping` | 保活 |

板 → 上位机：

| 事件 | 说明 |
| --- | --- |
| `hello` | 上线声明能力（musicbox 版本、槽位数、歌曲列表） |
| `gen_req {type: instrumental\|song}` | S1/S2 触发生成 |
| `hum_req` | S5 触发哼歌成曲（上位机随后 `rec_pull`） |
| `prompt_req` | S6 触发提示词生成（上位机随后 `rec_pull` + ASR） |
| `rec_meta` / `rec_chunk` / `rec_end` | 录音上传分片 |
| `song_list` / `state` | 歌曲库与播放状态同步 |

### 3.5 S7/S8 设计：切歌与 Wi-Fi 传歌（蓝牙已弃用）

**弃用蓝牙的原因**：ESP32-S3 只有 BLE 无经典蓝牙，手机系统的"蓝牙收文件"（OPP）不可用，iOS 也无蓝牙文件传输概念；BLE 传文件必须依赖会讲私有协议的接收端，体验和速度（20~50KB/s，3MB 约 1~3 分钟）都差。

**S7 切歌**：歌曲库维护"当前曲目"索引；播放中按 S7 → 索引 +1（到尾回卷到第一首）并立即播放；仅一首歌时为重播。板端独立完成，不依赖任何连接。

**S8 Wi-Fi 热点传歌**：

- S8 短按 = 热点开/关切换（开：Wi-Fi SoftAP，SSID 如 `MusicBox-XXXX`，免密或固定密码；再按关闭省电）
- 开热点后板载 `esp_http_server` 提供内置网页：
  - `GET /` → 歌曲列表页（标题/时长/大小，HTML 内嵌固件，免配套软件）
  - `GET /song/{n}.mp3` → 直接下载对应 MP3（HTTP Range 可选）
- 手机连接热点后浏览器访问 `192.168.4.1` 即可浏览/下载，iOS/Android 通吃，3MB 歌曲秒级完成
- 手机连热点期间无外网 → 该页面只承担取歌，不承担 AI 生成
- 可选增强：mDNS（`musicbox.local`）、Captive Portal 自动弹出下载页

功耗约束：Wi-Fi 功耗高于 BLE，热点**仅在 S8 开启期间工作**，下载完手动/超时自动关闭（超时时长项目自定）。

---

## 4. 配套网页设计（`easyinput-musicbox/app/`）

Vite 网页结构，通信层精简为 **Web Serial 单通道**（USB，JSON 行协议）。

### 4.1 音乐生成 Provider 可插拔层

统一接口：

```
generate({ mode: "instrumental" | "song", prompt, refAudio? }) → audioURL
```

- 首个实现：`stepfun.js`（**阶跃星辰**，已对接）

### 4.2 阶跃音乐 API 对接（已确认，文档：https://platform.stepfun.com/docs/zh/api-reference/audio/music）

- 域名 `https://api.stepfun.com`，鉴权 `Authorization: Bearer <API Key>`（网页端输入后存 localStorage）
- 异步任务：`POST /v1/audio/music/submit` 返回 `task_id` → 每 5s `POST /v1/audio/music/query` → `status=SUCCESS` 时**音频以 Base64 随响应返回**（非 URL，网页端转 Blob/ObjectURL）；终态 `SUCCESS/FAILED`，失败按 `error.stage` 处理（`censor`=审核拦截等）
- 模型固定 `stepaudio-3-music-preview`；统一 `response_format=mp3`；输出原生 48kHz 立体声
- 功能映射：

| 功能 | task | 关键参数 |
| --- | --- | --- |
| S1 纯音乐 | `text_to_music` | `instrumental=true, caption=prompt` |
| S2 带歌词 | `text_to_music` | `caption=prompt`（不传 lyrics 自动写词） |
| S5 哼歌成曲 | `vocal_to_music` | `vocal_audio`=录音 Base64（旋律跟随哼唱、保留音色）；`lyrics` 必填 → 哼唱无词，用"啦"音节占位歌词；`caption` 描述编曲风格 |
| S6 提示词 | ASR 转文字后同 S1 | — |

- 跨域：开发期经 Vite dev proxy（`/stepfun` → 阶跃域名），生产可用 `VITE_STEPFUN_BASE` 覆盖
- 错误处理按 `error.type`（402=余额不足、429=限流等），不匹配文本
- 生成内容对外分发需按法规添加 AI 生成标识

### 4.3 各按键对应流程

| 按键 | 网页侧流程 |
| --- | --- |
| S1 | 内置随机风格提示词池 → `generate(instrumental)` → 下载 → 转码 → 写板 |
| S2 | 随机主题+歌词提示词 → `generate(song)` → 同上 |
| S5 哼歌成曲 | `rec_pull` 拉录音 → **优先音潮参考音频仿写能力**（直接 API 路线，先看效果）；备选：basic-pitch 在浏览器端转 MIDI/旋律后再生成 |
| S6 | `rec_pull` 拉录音 → ASR（Web Speech API 或三方 ASR）→ 文字作提示词 → `generate(instrumental)` |
| S7/S8 | 板端独立完成（切歌 / 热点开关 + 内置下载页），配套网页不参与 |

- 音频转码：如需统一为 128k MP3，用 ffmpeg.wasm 在浏览器端完成。
- 生成期间下发 `gen_state:start`（板端跑马灯），写板完成后发 `gen_state:done`（板端绿闪 3 次）。

---

## 5. 里程碑

| # | 内容 | 产出 |
| --- | --- | --- |
| M1 | 固件骨架：电源顺序 + 按键/旋钮/灯效 + REPL 自检（keytest 模式） | ✅ 完成（2026-10-14，build 270KB，基线扫描 10 PASS） |
| M2 | 录音（S3/S4）+ MP3 播放 + 歌曲分区读写 + 歌曲库 | ✅ 完成编译验证（2026-10-15，build 340KB/余 83%，Helix=chmorgan/esp-libhelix-mp3，待实板验证） |
| M3 | 协议 + 网页通信打通：S1/S2 全流程（阶跃真实 provider 已就位） | ✅ 完成编译验证（2026-10-15，二进制分块帧 0xAA55+CRC32 停等 ACK，固件 352KB，双端构建通过，待实板验证） |
| M4 | S6（ASR→提示词）、S5（哼歌成曲，阶跃 vocal_to_music 路线） | ✅ 完成编译验证（2026-10-15，rec_pull 录音回传 + WAV 封装 + stepaudio-2.5-asr，固件 359KB，双端构建通过，待实板验证） |
| M5 | S7 切歌 + S8 Wi-Fi 热点与板载下载页（手机浏览器实测 iOS/Android） | ✅ 完成编译验证（2026-10-15，SoftAP MusicBox-XXXX 免密 + esp_http_server 列表页/`/song/{n}.mp3` 下载(支持 Range) + 极简 DNS 实现 Captive Portal + 10min 空闲自动关热点，固件 934KB，基线 10 PASS，待实板验证） |
| M6 | 全功能联调验收（含实板音频链路验证） | 全功能验收 |

---


