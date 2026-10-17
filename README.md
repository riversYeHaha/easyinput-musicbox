# EasyInput MusicBox

一块巴掌大的 AI 随身音乐盒：**ESP32-S3 开发板** + **USB 串口配套网页**。
板上按键触发，网页经 Web Serial 直连开发板，调用阶跃星辰（StepFun）音乐生成
API，把成品 MP3 分块写回板载 flash 播放；还能一键开热点，用手机连上直接取歌。

板型：EasyInput V2.0/v2 · PCB AI Keyboard V2.1 · **ESP32-S3R8** · 16MB Flash

## 仓库结构

```
firmware/        ESP-IDF v5.5.5 工程（C，main/ 为核心模块）
  main/          按键、编码器、录音、播放、歌曲库、串口协议、WiFi 热点分享、LED、电源
  partitions.csv 16MB 分区表（nvs / factory / rec / songs）
  sdkconfig.defaults  目标与分区默认值（实际 sdkconfig 不入库）
app/             Vite + 原生 JS 配套网页（无框架）
  index.html     全部 UI 样式与结构
  src/main.js    串口收发、二进制帧、任务队列、歌曲库、生成控制台、S6 识别来源
  src/provider/  阶跃 API 封装（stepfun.js）+ 本地/云端 ASR
  vite.config.js /stepfun 代理 + 本地 SenseVoice ASR 桥接插件
  README.md      网页端与 StepFun API 的详细文档
docs/            实现方案与开发经验总结留档
```

## 硬件能力（板上按键）

| 键 | 短按 | 长按 |
|---|---|---|
| S1 | 随机生成纯音乐 | — |
| S2 | 随机生成带歌词歌曲 | — |
| S3 | — | 按住录音（红灯），松开停止 |
| S4 | 回放最近录音（再按停） | — |
| S5 | 哼歌成曲（哼一段→AI 配乐） | — |
| S6 | 说话转文字作提示词→生成**带歌词演唱**歌曲 | — |
| S7 | 切歌（到尾回卷；仅一首时重播） | — |
| S8 | 开/关 WiFi 热点取歌页 | — |

## 固件：构建与烧录

依赖 [ESP-IDF v5.5.5](https://docs.espressif.com/projects/esp-idf/)。注意本工程
需把 IDF 的 Python 环境固定到 3.13 venv，否则会退回 3.9 触发全量重编：

```bash
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/tools/python/v5.5.5/venv"
. "$HOME/.espressif/v5.5.5/esp-idf/export.sh"
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

分区规划（16MB）：`factory` 固件 2MB（含 Helix MP3 解码器余量）、`rec` 1MB（约 30 秒
16kHz/16bit/单声道录音）、`songs` 约 13MB 等分 4 槽存生成的 MP3（每槽约 3.2MB），
槽头 + NVS 维护当前曲目与全局序号。

## 配套网页：运行

```bash
cd app
pnpm install        # 或 npm install
pnpm dev            # 开发服务器：含 /stepfun 代理 + 本地 ASR 桥接
```

用支持 Web Serial 的 Chromium 内核浏览器（Chrome / Edge）经 USB 连开发板；API Key
在页面顶栏填入并保存（存浏览器 localStorage，不落仓库）。详见 [app/README.md](app/README.md)。

## 通信协议（网页 ↔ 板）

Web Serial 上混用两种帧：JSON 行协议（`\n` 分隔，控制/事件）与二进制分块帧
（`0xAA 0x55 | seq | len | payload | crc32`）。写歌采用「三段式 + 停等 ACK」流式落盘，
录音回传采用同款二进制帧从 `rec` 分区拉取。CRC32 为反射多项式 0xEDB88320。

## 关键特性

- **哼歌成曲（S5）**：网页侧对录音做峰值归一化（只升不降、封顶 4×）与信号诊断，
  缓解采集偏轻导致成品偏离原哼唱。
- **S6 双识别引擎**：本地 **SenseVoice**（AniShort 下载的 funasr 模型，离线、不吃云端
  额度）或阶跃云端 `stepaudio-2.5-asr`，顶栏每次可切换；识别文本直接作为 prompt 触发
  生成。桥接以 Vite 插件形式挂同源 `POST /localasr/transcribe`，仅 `dev` 生效。
- **S8 热点取歌页**：板子开 AP，手机连上打开 `192.168.4.1`，每首歌「播放 / 下载」双
  按钮。下载经 `Content-Disposition` 强制保存（文件名取标题 ASCII slug，中文标题回落
  `song-N.mp3`）；服务端对每段响应给出精确 `Content-Length` 与 Range/206，兼容 Safari
  的播放与下载。热点空闲 10 分钟自动关闭。

## 许可与致谢

音乐生成与 ASR 依赖阶跃星辰开放平台；本地识别依赖达摩院 SenseVoice（经 FunASR）。
仓库不含任何密钥或凭证。
