/*
 * 阶跃星辰音乐生成 Provider
 * 文档：https://platform.stepfun.com/docs/zh/api-reference/audio/music
 *
 * 统一接口：generate({ mode, prompt?, refAudio? }, onProgress?) →
 *   { audioBlob, audioUrl(objectURL), title, lyric, size }
 *   mode = "instrumental"  S1/S6 纯音乐  → task=text_to_music + instrumental=true
 *   mode = "song"          S2 带歌词歌曲 → task=text_to_music（自动写词）
 *   mode = "reference"     S5 哼歌成曲   → task=vocal_to_music（旋律跟随哼唱、保留音色）
 *
 * 鉴权：Bearer Token，Key 存 localStorage（页面顶部输入）。
 * 跨域：开发期经 vite dev proxy（/stepfun → api.stepfun.com），见 vite.config.js。
 * 异步：submit 得 task_id，POST query 每 5s 轮询至 SUCCESS，音频以 Base64 随响应返回。
 */

const BASE = import.meta.env?.VITE_STEPFUN_BASE || '/stepfun';
const MODEL = 'stepaudio-3-music-preview';
const KEY_STORE = 'musicbox.stepfun_api_key';

export const POLL_INTERVAL_MS = 5000; // 官方建议每 5 秒
export const POLL_AUTO_MS = 600000; // 单次自动轮询窗口；超时后任务仍可凭 taskId 手动查询
const PENDING_HINT_MS = 90000; // PENDING 持续此时长提示平台繁忙

/* 哼唱无实际歌词，vocal_to_music 要求 lyrics 与演唱内容一致 → "啦"音节占位 */
const HUM_PLACEHOLDER_LYRICS = '[Verse 1]\n啦 啦啦 啦啦啦\n啦啦 啦啦啦\n[Chorus 1]\n啦啦啦 啦啦 啦啦啦';

const ASR_MODEL = 'stepaudio-2.5-asr';

/* 失败阶段 → 人话提示（S5 哼唱常见问题） */
const STAGE_HINTS = {
  no_speech_detected: '没检测到人声，哼唱可能太轻或太短，请靠近麦克风重哼一段',
  audio_transcode: '录音格式无法转码，请确认采样率/格式',
  audio_format_unsupported: '不支持的音频格式',
};

export function setApiKey(key) {
  localStorage.setItem(KEY_STORE, key.trim());
}
export function getApiKey() {
  return localStorage.getItem(KEY_STORE) || '';
}

async function request(path, body) {
  const resp = await fetch(BASE + path, {
    method: 'POST',
    headers: {
      Authorization: `Bearer ${getApiKey()}`,
      'Content-Type': 'application/json; charset=utf-8',
    },
    body: JSON.stringify(body),
  });
  const data = await resp.json().catch(() => ({}));
  if (!resp.ok) {
    // 统一错误结构 {error:{message,type}}；按 error.type 判断，不匹配文本
    const type = data?.error?.type || `http_${resp.status}`;
    throw new Error(`阶跃 API 错误 [${type}]: ${data?.error?.message || resp.status}`);
  }
  return data;
}

function blobToBase64(blob) {
  return new Promise((resolve, reject) => {
    const r = new FileReader();
    r.onload = () => resolve(r.result.split(',')[1]); // 去掉 data: 前缀
    r.onerror = reject;
    r.readAsDataURL(blob);
  });
}

function base64ToBlob(b64, mime = 'audio/mpeg') {
  const bin = atob(b64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return new Blob([bytes], { type: mime });
}

/* 提交生成任务，返回 task_id（任务列表凭此 id 可随时查询/恢复） */
export async function submitGeneration({ mode, prompt, refAudio }) {
  const common = { model_id: MODEL, response_format: 'mp3' };
  if (mode === 'instrumental') {
    const data = await request('/v1/audio/music/submit', {
      ...common, task: 'text_to_music', caption: prompt, instrumental: true,
    });
    return data.task_id;
  }
  if (mode === 'song') {
    const data = await request('/v1/audio/music/submit', {
      ...common, task: 'text_to_music', caption: prompt,
    });
    return data.task_id;
  }
  if (mode === 'reference') {
    if (!refAudio) throw new Error('哼歌成曲需要 refAudio（录音 Blob）');
    const data = await request('/v1/audio/music/submit', {
      ...common,
      task: 'vocal_to_music',
      caption: prompt || '轻快流行编曲，配器饱满',
      lyrics: HUM_PLACEHOLDER_LYRICS,
      vocal_audio: await blobToBase64(refAudio),
    });
    return data.task_id;
  }
  throw new Error(`未知 mode: ${mode}`);
}

/* ============ 控制台原始接口（板无关，手动传任意参数） ============
 * 供"音乐生成控制台"直接透传阶跃参数用，绕过 submitGeneration 的模式映射。
 * 参考音频字段（vocal_audio / song_audio）需调用方先用 blobToBase64 转好。 */
export async function submitRaw(params) {
  return await request('/v1/audio/music/submit', params);
}
export async function queryTaskRaw(taskId) {
  return await request('/v1/audio/music/query', { task_id: taskId });
}
export { blobToBase64 };

/* 轮询窗口超时（非业务失败）：任务在平台侧仍可能完成，凭 taskId 可继续查询 */
export class PollTimeoutError extends Error {
  constructor(taskId, lastStatus) {
    super('生成轮询超时（任务仍有效，可手动刷新状态）');
    this.name = 'PollTimeoutError';
    this.taskId = taskId;
    this.lastStatus = lastStatus;
  }
}

function failMessage(data) {
  const stage = data?.error?.stage || 'unknown';
  const hint = STAGE_HINTS[stage] ? `（${STAGE_HINTS[stage]}）` : '';
  return `生成失败 [${stage}]${hint}: ${data?.error?.message || ''}`.trim();
}

/* 单次查询任务，规范化为 { status, song?, error? }；不做任何重试/等待 */
export async function queryTask(taskId) {
  const data = await request('/v1/audio/music/query', { task_id: taskId });
  const out = { status: data.status, data };
  if (data.status === 'SUCCESS') {
    // 极少数情况下响应 SUCCESS 但 audio 缺失（如结果已过期被回收），给出可操作错误
    if (!data.audio) {
      out.status = 'FAILED';
      out.error = '平台返回成功但缺少音频数据（结果可能已过期），请重新生成';
      return out;
    }
    const audioBlob = base64ToBlob(data.audio);
    out.song = {
      audioBlob,
      audioUrl: URL.createObjectURL(audioBlob),
      title: data.rewritten_caption || data.caption || '未命名',
      lyric: data.rewritten_lyrics || data.lyrics || '',
      size: audioBlob.size,
    };
  } else if (data.status === 'FAILED') {
    out.error = failMessage(data);
  }
  return out;
}

/*
 * 自动轮询任务直到终态：
 *   SUCCESS → 返回 song；FAILED → throw Error；超过 timeoutMs 仍排队/运行 →
 *   throw PollTimeoutError（可恢复，调用方保留 taskId，之后可 queryTask 或再次轮询）。
 * 回调：onStatus(status) 仅状态变化时触发；onInfo(msg) 用于诊断/排队提示。
 * 参数均可通过 opts 覆盖（intervalMs/timeoutMs），方便后续调整。
 */
export async function pollGeneration(taskId, opts = {}) {
  const timeoutMs = opts.timeoutMs ?? POLL_AUTO_MS;
  const intervalMs = opts.intervalMs ?? POLL_INTERVAL_MS;
  const onStatus = opts.onStatus;
  const onInfo = opts.onInfo;
  const deadline = Date.now() + timeoutMs;
  let lastStatus = null;
  let firstDump = false;
  let pendingSince = 0;
  let pendingHinted = false;
  for (;;) {
    const q = await queryTask(taskId);
    if (!firstDump) {
      firstDump = true;
      onInfo?.(`首次响应 ${JSON.stringify({ ...q.data, audio: undefined })}`);
    }
    if (q.status !== lastStatus) {
      if (q.status === 'PENDING') pendingSince = Date.now();
      pendingHinted = false;
      onStatus?.(q.status);
      lastStatus = q.status;
    }
    if (q.status === 'SUCCESS') return q.song;
    if (q.status === 'FAILED') throw new Error(q.error);
    if (q.status === 'PENDING' && !pendingHinted && Date.now() - pendingSince > PENDING_HINT_MS) {
      pendingHinted = true;
      onInfo?.('排队超过 90 秒，preview 模型可能繁忙，继续等待中…');
    }
    if (Date.now() > deadline) throw new PollTimeoutError(taskId, q.status);
    await new Promise(r => setTimeout(r, intervalMs));
  }
}

/*
 * 录音转文字（S6）：POST /v1/audio/transcriptions，multipart/form-data，
 * 字段 model=stepaudio-2.5-asr、response_format=json、file=<wav>；返回 { text }。
 * 支持 wav/pcm/mp3/ogg；与生成同 host，复用 /stepfun 代理，无需改 vite。
 */
export async function transcribe(blob) {
  if (!getApiKey()) throw new Error('请先在页面顶部填入阶跃 API Key');
  const fd = new FormData();
  fd.append('model', ASR_MODEL);
  fd.append('response_format', 'json');
  fd.append('file', blob, 'rec.wav');
  const resp = await fetch(BASE + '/v1/audio/transcriptions', {
    method: 'POST',
    // 不手设 Content-Type：浏览器需自带 multipart boundary
    headers: { Authorization: `Bearer ${getApiKey()}` },
    body: fd,
  });
  const data = await resp.json().catch(() => ({}));
  if (!resp.ok) {
    // 统一错误结构 {error:{message,type}}；按 error.type 判断，不匹配文本
    const type = data?.error?.type || `http_${resp.status}`;
    throw new Error(`ASR 错误 [${type}]: ${data?.error?.message || resp.status}`);
  }
  return data.text || '';
}

/*
 * 本地 SenseVoice 录音转文字（S6）：走 Vite dev server 的同源桥接
 * POST /localasr/transcribe，body 为 WAV 原始字节，返回 { text }。
 * 桥接在 vite.config.js 的 localAsrBridge 插件里，调用本机 funasr CLI，
 * 离线、不占云端额度。可选 x-asr-language 头（zh/en/ja/ko/yue/auto）。
 */
export async function transcribeLocal(blob, language = 'auto') {
  const resp = await fetch('/localasr/transcribe', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/octet-stream',
      'x-asr-language': language,
    },
    body: blob,
  });
  const data = await resp.json().catch(() => ({}));
  if (!resp.ok) {
    throw new Error(`本地 ASR 错误: ${data?.error || resp.status}`);
  }
  return data.text || '';
}
