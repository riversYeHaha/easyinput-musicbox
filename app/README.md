# EasyInput MusicBox · 配套网页 App

浏览器直连开发板（Web Serial / USB CDC），调用阶跃星辰（StepFun）音乐生成 API，
把生成结果分块写回板载 `songs` 分区播放。技术栈：原生 JS + Vite，无框架。

- 入口：`index.html`（全部 UI 样式与结构都在这里，业务逻辑在 `src/main.js`）
- 逻辑：`src/main.js`（串口收发、二进制帧、任务队列、歌曲库、生成控制台）
- Provider：`src/provider/stepfun.js`（阶跃 API 封装 + 本地/云端 ASR 调用）
- 代理：`vite.config.js` 把 `/stepfun/*` 转发到 `https://api.stepfun.com`（规避跨域）
- 本地 ASR 桥：`vite.config.js` 内 `local-asr-bridge` 插件挂同源 `POST /localasr/transcribe`，
  调用本机 AniShort 下载的 funasr CLI（SenseVoice），供 S6 录音转文字离线使用（详见下文）

## 运行

```bash
npm run dev        # 开发服务器（含 /stepfun 代理），改代码即时生效
npm run build      # 产物输出到 dist/
npm run preview    # 预览 dist/
```

浏览器需为支持 Web Serial 的 Chromium 内核（Chrome / Edge），用 USB 连接开发板。
API Key 在页面顶部填入并保存（存 `localStorage`，key = `musicbox.stepfun_api_key`）。

## 功能分区

左侧面板三个 Tab：

- **生成队列**：板端 S1/S2/S5/S6 触发的生成任务；自动轮询，停止后可反复点「刷新状态」
  手动查询，成功即出现「写入开发板」。
- **歌曲库**：连接后自动同步板载歌曲；写板落盘后即时刷新。
- **生成控制台**：见下。

右侧为可折叠实时日志（含串口通信与每步诊断），可一键清空。

## 音乐生成控制台（手动全参数）

用于脱离开发板、直接对比各类生成效果：把阶跃接口的所有参数做成可手动输入的表单，
点「提交生成」拿到 `task_id`，再点「查询状态」单次拉取最新结果；生成成功后可
**播放 / 下载 / 写入开发板**。参考音频通过本地文件选择器提供（运行时选择，不打包进应用）。

字段与下方 API 参数一一对应；数值类参数留空即「不传，用平台默认」。

## StepFun 音乐生成 API 参考

> 官方文档：<https://platform.stepfun.com/docs/zh/api-reference/audio/music>
> 以下为要点留档（截至 2026-10），接口若有更新以官方文档为准。

### 调用模式：异步「提交 + 查询」

1. `POST /v1/audio/music/submit` → 返回 `{ "task_id": "..." }`
2. 每约 5 秒 `POST /v1/audio/music/query`，body `{ "task_id": "..." }`，
   直到 `status` 变为 `SUCCESS` 或 `FAILED`。
3. 成功时 `audio` 字段为 **Base64** 音频内容（随响应返回）。

请求头：`Authorization: Bearer $STEP_API_KEY`，`Content-Type: application/json; charset=utf-8`。

### 任务类型（task）

| task | 说明 | 必填 |
|---|---|---|
| `text_to_music` | 文生带词歌曲 | `caption`（`lyrics` 可选，不传服务端可能自动写词） |
| `text_to_music` + `instrumental=true` | 文生纯器乐 | `caption`、`instrumental=true`；**不能传 `lyrics`** |
| `vocal_to_music` | 干声/哼唱配乐（旋律主要跟随干声、保留音色） | `caption`、`lyrics`、`vocal_audio` |
| `music_cover` | 翻唱（旋律跟随参考歌曲） | `caption`、`lyrics`、`song_audio` |

`vocal_to_music` 关键约束：**干声演唱内容应是 `lyrics` 的子集或全集**——哼唱场景里
`lyrics` 与哼唱旋律对齐度越高，跟随越好。

### 请求参数

| 参数 | 类型 | 说明 |
|---|---|---|
| `task` | string | 必填，任务类型 |
| `model_id` | string | 必填，固定 `stepaudio-3-music-preview` |
| `caption` | string | 必填，风格描述（曲风 / 人声 / 律动 / 情绪 / 调式等） |
| `lyrics` | string | 可选；`music_cover`、`vocal_to_music` 必填。支持结构标签：`[Intro] [Verse 1] [Pre-Chorus] [Chorus 1] [Bridge] [Instrumental] [Hook] [Break] [Drop] [Ad-lib] [Outro]` |
| `instrumental` | bool | 仅 `text_to_music` 支持；为 true 时不能传 `lyrics` |
| `song_audio` | string | 参考歌曲 **Base64**，`music_cover` 必填 |
| `vocal_audio` | string | 干声 **Base64**，`vocal_to_music` 必填 |
| `response_format` | string | 输出格式：`wav` `flac` `opus` `mp3` `pcm`（推荐 `mp3`，体积小） |
| `sample_rate` | number | 输出采样率，默认 `48000` |
| `bit_rate` | number | MP3 / Opus 比特率 |
| `temperature` | number | 采样温度 |
| `top_k` | number | Top-K 采样 |
| `top_p` | number | Top-P 采样 |
| `repetition_penalty` | number | 重复惩罚 |
| `max_tokens` | number | 生成 token 上限 |

**音频输入字段格式**：`song_audio` / `vocal_audio` 均传「原始文件字节的 Base64 编码，
不含 `data:` 前缀」。`pcm` 为无头裸流，需自行按采样率/位深/声道解析。

### 查询响应

成功：`status=SUCCESS`，含 `task`、`caption`、`lyrics`、`response_format`、`sample_rate`、
`audio`(Base64)、`rewritten_caption`、`rewritten_lyrics`。

状态：`PENDING`（已入队）/ `RUNNING`（处理中）/ `SUCCESS` / `FAILED`。
`FAILED` 是业务终态但 HTTP 仍可能是 200；失败原因见 `error.stage`。

失败阶段（`error.stage`）：

| stage | 含义 |
|---|---|
| `audio_transcode` | 音频解码 / 转码 / 采样率非法 |
| `no_speech_detected` | 干声中未检测到有效人声（哼唱太轻/太短/太杂） |
| `censor` | 内容被安全审核拦截 |
| `internal` | 服务端内部错误 |

### 提交接口常见错误码

| HTTP | `error.type` | 含义 |
|---:|---|---|
| 400 | `request_params_invalid` | 参数非法或缺必填 |
| 401 | `not_allowed` | 鉴权失败 |
| 402 | `insufficient_credit` | Credit 不足 |
| 404 | `model_invalid` | 模型 ID 错误或无权限 |
| 429 | `rate_limited` | 速率限制 |
| 503 | `service_unavailable` | 服务不可用 |

查询接口另有 `400 request_params_invalid`（缺 `task_id`）、`404 task_not_found`、
`429 rate_limited`、`503 service_unavailable`。

## 录音转文字（S6）：本地 SenseVoice / 阶跃云端

板上按 **S6** → 拉取录音 → ASR 转文字 → 文字作为 `caption`（风格/提示词）提交，
生成**带歌词演唱**的歌曲（`mode: song`，即 `text_to_music` 不带 `instrumental`，
歌词由平台按 caption 自动撰写并演唱；识别结果不再二次确认，自动进入生成）。

顶栏「S6 识别」单选决定用哪个引擎（每次生效，默认**本地 SenseVoice**）：

- **本地 SenseVoice**（推荐，离线、不占云端额度）
- **阶跃云端** `stepaudio-2.5-asr`（本地不可用或想要更高质量时切换）

### 本地 SenseVoice 桥接原理

浏览器无法直接执行命令行，故在 `vite.config.js` 的 `local-asr-bridge` Vite 插件里挂一个
**同源** `POST /localasr/transcribe` 中间件：收 WAV 原始字节 → 写临时文件 → 调用本机
AniShort 下载的 `funasr` CLI（模型 **SenseVoiceSmall**）→ 清洗 stdout → 返回 `{ text }`。

> 该桥接随 `npm run dev` 起，属 dev server 能力；`npm run build` 出的静态 `dist/` **不含**
> 此中间件。生产如需本地识别，请自备等价服务或改用阶跃云端。

关键点：

- `funasr` 是 AniShort 内嵌 Python 的入口脚本，其 `site-packages` 不在默认 `sys.path`，
  直接执行会 `ModuleNotFoundError: funasr`。桥接会显式注入 `PYTHONPATH`，并把
  `MODELSCOPE_CACHE` 指向已下载模型目录（`~/.anishort/models/modelscope`）避免重下。
- **输入格式无需重采样**：SenseVoice 要求 16kHz 单声道，正好等于板端录音参数
  （16000Hz / 16bit / mono），浏览器 `pcmToWav` 产出的 WAV 可直接喂入。
- 插件自动探测环境；探测不到时端点返回 **501** 并提示切换云端，其余功能不受影响。
  单次调用约在 CLI 里冷加载 ~1GB 权重（总耗时约数秒），并对并发请求做串行排队防内存尖峰。

可用环境变量覆盖探测结果（例如 AniShort 路径变化或多版本 Python）：

```bash
MUSICBOX_FUNASR_BIN=~/.anishort/python-userbase/bin/funasr      # CLI 入口
MUSICBOX_MODELSCOPE_CACHE=~/.anishort/models/modelscope         # 模型缓存目录
```

冒烟测试（dev 起来后直接 curl，无需连板）：

```bash
# 任取一段 16k/单声道 WAV；也可从模型自带样例转：
ffmpeg -y -i zh.mp3 -ar 16000 -ac 1 -c:a pcm_s16le /tmp/t.wav
curl -s -X POST "http://localhost:5173/localasr/transcribe" \
     -H 'x-asr-language: zh' --data-binary @/tmp/t.wav
# → {"text":"…识别出的中文…"}
```

`x-asr-language` 可选 `zh / en / ja / ko / yue / auto`（不传按 `auto`）。本地识别失败时，
日志会给出 `本地识别失败：…；可在顶栏切「阶跃云端」后重按 S6` 的提示。

### 阶跃云端 ASR 接口（备选引擎）

`POST /v1/audio/transcriptions`，`multipart/form-data`，字段：`model=stepaudio-2.5-asr`、
`response_format=json`、`file=<wav>`（与生成同 host，复用 `/stepfun` 代理）；返回 `{ text }`。

常见错误 `error.type`：

| type | 含义 |
|---|---|
| `quota_exceeded` | 当前账号额度用尽（`You exceeded your current quota...`）——ASR 模型额度与音乐生成额度可能分开计，需去阶跃控制台确认 `stepaudio-2.5-asr` 是否已开通/充值 |
| `insufficient_credit` | Credit 不足 |
| `not_allowed` | 鉴权失败 |
| `model_invalid` | 模型 ID 错误或无权限 |

遇到 `quota_exceeded` 时，切到**本地 SenseVoice**即可绕开云端额度继续用 S6。

## 哼歌成曲（S5）调优备忘

链路（板 → 网页 → API）已逐段核对正确：板载 16k/16bit/单声道录音、`rec_meta` 如实报率、
WAV 头无误、`vocal_audio` 字段与裸 Base64 格式符合文档。若成品仍偏离原哼唱，多为
**采集信号偏弱**：诊断日志 `[rec·诊断]` 会打印时长/峰值/RMS，`RMS < 约 2%` 视为太轻。
网页侧已加「峰值归一化」把轻哼抬到接近满量程（只升不降、封顶 4×），但仍建议哼得连贯、
离麦更近。要更强效果可考虑后续升级为「仅有声段做自动增益(AGC)」。
