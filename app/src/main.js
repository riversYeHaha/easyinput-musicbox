/*
 * EasyInput MusicBox 配套网页
 * 通信：Web Serial（USB CDC），115200 8N1，JSON 行协议（\n 分隔）。
 * 生成：阶跃 provider（见 provider/stepfun.js）。
 * M3 写板：二进制分块帧 + 停等 ACK（见下方 writeSongToBoard）。
 * M4 录音回传：rec_pull → rec_meta + 同款二进制帧 + rec_end（见下方 pullRecording）。
 */
import * as provider from './provider/stepfun.js';

const BAUD = 115200;

/* ---- M3 写板参数（与固件 proto.c 一致） ---- */
const CHUNK = 4096; // 每块字节数（固件 SONG_CHUNK_MAX）
const ACK_TIMEOUT_MS = 3000; // 单块 ACK 超时
const MAX_RETRY = 3; // 单块最多重发次数
const BEGIN_TIMEOUT_MS = 90000; // song_begin 需等板端擦除槽位（3MB 全擦约数十秒）

/* S1/S2 随机提示词池（后续可扩充/自定义） */
const PROMPT_POOLS = {
  instrumental: [
    '轻快的原声吉他 BGM，适合咖啡馆氛围',
    '舒缓的钢琴曲，雨天窗边的安静情绪',
    '活泼的八音盒旋律，童话感，适合清晨',
    '慵懒的 Lo-Fi 节拍，适合学习和放松',
    '明快的尤克里里小调，夏日海边气息',
  ],
  song: [
    '一首轻快的流行歌曲，主题是夏日海边的回忆',
    '一首温暖的民谣，关于老朋友重逢',
    '一首元气满满的电子流行歌，主题是周末出游',
    '一首治愈系慢歌，关于夜晚的城市灯光',
  ],
};
const pick = arr => arr[Math.floor(Math.random() * arr.length)];

/* CRC32：标准实现（poly 0xEDB88320，初值 0xFFFFFFFF，末态取反），与固件 proto.c 同一实现 */
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[i] = c >>> 0;
  }
  return t;
})();

function crc32(bytes) {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) {
    c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  }
  return (c ^ 0xffffffff) >>> 0;
}

const logEl = document.getElementById('log');
const statusEl = document.getElementById('status');
const connectBtn = document.getElementById('connect');
const apikeyEl = document.getElementById('apikey');
const savekeyBtn = document.getElementById('savekey');

apikeyEl.value = provider.getApiKey();
savekeyBtn.addEventListener('click', () => {
  provider.setApiKey(apikeyEl.value);
  log('[key] 已保存到 localStorage');
});

let port = null;
let writer = null;

function log(...args) {
  const line = args.map(a => (typeof a === 'string' ? a : JSON.stringify(a))).join(' ');
  logEl.textContent += line + '\n';
  logEl.scrollTop = logEl.scrollHeight;
  console.log(...args);
}

/* 发送一条 JSON 行（上位机 → 板）；串口异常只打日志不抛出（runGenerate 为后台任务） */
async function send(obj) {
  if (!writer) {
    log('[send] not connected, dropped:', obj);
    return;
  }
  try {
    await writer.write(new TextEncoder().encode(JSON.stringify(obj) + '\n'));
  } catch (e) {
    log('[send error]', String(e));
  }
}

/* ---- 板端应答等待器：等满足条件的下一条 JSON 行（带超时） ---- */
const waiters = []; // { pred, resolve, reject, timer }

function waitFor(pred, timeoutMs, label) {
  return new Promise((resolve, reject) => {
    const w = { pred, resolve, reject };
    w.timer = setTimeout(() => {
      const i = waiters.indexOf(w);
      if (i >= 0) waiters.splice(i, 1);
      reject(new Error(`等待板端回复超时: ${label}`));
    }, timeoutMs);
    waiters.push(w);
  });
}

function serveWaiters(ev) {
  for (let i = waiters.length - 1; i >= 0; i--) {
    const w = waiters[i];
    if (w.pred(ev)) {
      waiters.splice(i, 1);
      clearTimeout(w.timer);
      w.resolve(ev);
    }
  }
}

function rejectAllWaiters(reason) {
  for (const w of waiters.splice(0)) {
    clearTimeout(w.timer);
    w.reject(new Error(reason));
  }
}

/* 拼一帧：0xAA 0x55 | seq u16le | len u16le | payload | crc32 u32le(payload) */
function buildFrame(seq, payload) {
  const frame = new Uint8Array(6 + payload.length + 4);
  frame[0] = 0xaa;
  frame[1] = 0x55;
  frame[2] = seq & 0xff;
  frame[3] = seq >>> 8;
  frame[4] = payload.length & 0xff;
  frame[5] = payload.length >>> 8;
  frame.set(payload, 6);
  const c = crc32(payload);
  frame[6 + payload.length] = c & 0xff;
  frame[7 + payload.length] = (c >>> 8) & 0xff;
  frame[8 + payload.length] = (c >>> 16) & 0xff;
  frame[9 + payload.length] = c >>> 24;
  return frame;
}

/*
 * 把整首歌写入板载歌曲库（M3）：
 * song_begin → 4KB 分块逐块停等 ACK（超时重发）→ song_end（整曲 CRC32）→ song_stored 事件。
 */
async function writeSongToBoard(title, bytes /* Uint8Array */) {
  const totalCrc = crc32(bytes);
  log(`[write] 《${title}》 ${(bytes.length / 1048576).toFixed(2)}MB，crc=${totalCrc.toString(16).padStart(8, '0')}`);

  await send({ cmd: 'song_begin', title, size: bytes.length });
  const begin = await waitFor(
    ev => ev.ack === 'song_begin' || 'err' in ev,
    BEGIN_TIMEOUT_MS,
    'song_begin（板端正在擦除槽位，可能需数十秒）',
  );
  if (begin.err) throw new Error(`song_begin 被拒: ${begin.err}${begin.msg ? '（' + begin.msg + '）' : ''}`);
  const total = Math.ceil(bytes.length / CHUNK);
  log(`[write] slot=${begin.slot}，共 ${total} 块，开始传输`);

  let nextLogPct = 10;
  for (let seq = 0; seq < total; seq++) {
    const payload = bytes.subarray(seq * CHUNK, Math.min(bytes.length, (seq + 1) * CHUNK));
    const frame = buildFrame(seq, payload);
    let acked = false;
    for (let attempt = 1; attempt <= MAX_RETRY; attempt++) {
      await writer.write(frame);
      let r = null;
      try {
        r = await waitFor(
          ev => ev.ack === seq || ev.nack === seq || 'err' in ev,
          ACK_TIMEOUT_MS,
          `chunk ${seq}`,
        );
      } catch { /* 单块超时，进入重发 */ }
      if (r && r.err) {
        throw new Error(`板端中止写板: ${r.err}${r.msg ? '（' + r.msg + '）' : ''}`);
      }
      if (r && r.ack === seq) {
        acked = true;
        break;
      }
      if (attempt < MAX_RETRY) {
        if (r && r.nack === seq) {
          log(`[write] chunk ${seq} 被板端拒收（${r.why || '?'}），重发 ${attempt}/${MAX_RETRY - 1}`);
          await new Promise(res => setTimeout(res, 50)); // 等板端清空残留输入字节
        } else {
          log(`[write] chunk ${seq} ACK 超时，重发 ${attempt}/${MAX_RETRY - 1}`);
        }
      }
    }
    if (!acked) throw new Error(`chunk ${seq} 重发 ${MAX_RETRY} 次仍失败，写板中止`);

    const pct = Math.floor(((seq + 1) / total) * 100);
    if (pct >= nextLogPct) {
      log(`[write] ${pct}%（${(seq + 1) * CHUNK > bytes.length ? bytes.length : (seq + 1) * CHUNK}/${bytes.length} 字节）`);
      nextLogPct += 10;
    }
  }

  await send({ cmd: 'song_end', crc: totalCrc });
  const end = await waitFor(ev => ev.ack === 'song_end' || 'err' in ev, 10000, 'song_end');
  if (end.err || !end.ok) {
    throw new Error(`song_end 校验失败: ${end.err || ''}${end.msg ? '（' + end.msg + '）' : ''}`.trim());
  }
  const stored = await waitFor(ev => ev.ev === 'song_stored', 10000, 'song_stored 事件');
  log(`[write] 已落盘 slot=${stored.slot} 《${stored.title}》，板端可离线播放`);
}

/* ================= 生成任务列表 =================
 * 每次 S1/S2/S5/S6 触发生成创建一条任务记录：
 *   SUBMITTING → PENDING/RUNNING → SUCCESS（可写板）/ FAILED
 *                    └─ 自动轮询窗口结束仍非终态 → TIMEOUT（保留 taskId，可手动"刷新状态"）
 * 超时不丢任务：taskId 一直在卡片上，刷新走单次 query，若仍在跑则恢复一整轮轮询。 */

const MODE_LABEL = { instrumental: '纯音乐', song: '带歌词歌曲', reference: '哼歌成曲' };
const STATUS_BADGE = {
  SUBMITTING: '提交中', PENDING: '排队中', RUNNING: '生成中',
  TIMEOUT: '待刷新', SUCCESS: '待写板', FAILED: '失败',
};
const genTasks = [];
let taskSeq = 0;
const tasksEl = document.getElementById('tasks');

function hhmmss(ts) {
  const d = new Date(ts);
  return [d.getHours(), d.getMinutes(), d.getSeconds()]
    .map(n => String(n).padStart(2, '0')).join(':');
}
function esc(s) {
  return String(s).replace(/[&<>"']/g,
    c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function createTask(mode, source) {
  const t = {
    seq: ++taskSeq, mode, source, taskId: null,
    status: 'SUBMITTING', msg: '正在提交生成任务…', song: null,
    createdAt: Date.now(), busy: false, writing: false, written: false, polling: false,
  };
  genTasks.unshift(t);
  renderTasks();
  return t;
}

function patchTask(t, p) {
  Object.assign(t, p);
  renderTasks();
}

function renderTasks() {
  if (!tasksEl) return;
  if (!genTasks.length) {
    tasksEl.innerHTML = '<div class="tempty">暂无生成任务<br>按板上 S1 / S2 / S5 / S6 触发</div>';
    return;
  }
  tasksEl.innerHTML = genTasks.map(t => {
    const shortId = t.taskId ? esc(t.taskId.slice(0, 8)) : '—';
    const sizeM = t.song ? (t.song.size / 1048576).toFixed(2) : '';
    let actions = '';
    if (t.status === 'SUBMITTING') {
      actions = '<span class="tdim">提交中…</span>';
    } else if (t.status === 'PENDING' || t.status === 'RUNNING' || t.status === 'TIMEOUT') {
      // 初次自动轮询进行中禁用（状态本就每 5s 自动更新）；轮询停止后=单次手动刷新，可反复点击
      const dis = t.busy || t.polling ? 'disabled' : '';
      actions = `<button data-act="refresh" data-seq="${t.seq}" ${dis}>刷新状态</button>`;
    } else if (t.status === 'SUCCESS') {
      // 兜底：SUCCESS 但音频缺失（如响应里无 audio 字段），不当作可写板处理
      actions = t.song
        ? `<a class="tlink" href="${t.song.audioUrl}" target="_blank">试听</a>`
          + (t.writing
            ? '<button disabled>写入中…</button>'
            : `<button data-act="write" data-seq="${t.seq}">${t.written ? '已写入 ✓' : '写入开发板'}</button>`)
        : '<span class="tdim">缺少音频数据，请点刷新重试</span>';
    }
    return `<div class="tcard st-${t.status}">
      <div class="thead">
        <span class="tmode">${esc(t.source || '')} · ${esc(MODE_LABEL[t.mode] || t.mode)}</span>
        <span class="tbadge">${STATUS_BADGE[t.status] || t.status}</span>
      </div>
      <div class="tmsg" title="${esc(t.msg)}">${esc(t.msg)}</div>
      <div class="tmeta">#${t.seq} · id=${shortId} · ${hhmmss(t.createdAt)}${sizeM ? ` · ${sizeM}MB` : ''}</div>
      <div class="tactions">${actions}</div>
    </div>`;
  }).join('');
}
renderTasks();

/* 自动轮询一条任务，结果写回任务记录（超时/失败均在内部消化，不向外抛） */
async function runTaskPolling(t) {
  t.polling = true;
  renderTasks();
  try {
    const song = await provider.pollGeneration(t.taskId, {
      // 只处理中间态：SUCCESS/FAILED 是终态，由下方拿到 song 或 catch 统一处理，
      // 否则会在 t.song 还是 null 时把卡片渲染成 SUCCESS
      onStatus: s => {
        if (s !== 'PENDING' && s !== 'RUNNING') return;
        patchTask(t, {
          status: s,
          msg: s === 'RUNNING' ? '平台生成中…' : '已入队，等待平台调度',
        });
      },
      onInfo: m => {
        log('[poll]', m);
        if (t.status === 'PENDING') patchTask(t, { msg: m });
      },
    });
    t.song = song;
    patchTask(t, { status: 'SUCCESS', msg: `《${song.title}》生成完成，点"写入开发板"落盘` });
    log(`[done] 《${song.title}》 ${(song.size / 1048576).toFixed(2)}MB`);
    if (song.lyric) log('[lyric]', song.lyric.slice(0, 120) + (song.lyric.length > 120 ? '…' : ''));
  } catch (e) {
    if (e instanceof provider.PollTimeoutError) {
      patchTask(t, { status: 'TIMEOUT', msg: '自动轮询超时（任务仍有效），点"刷新状态"查询结果' });
      log('[gen] 轮询超时，任务已保留在右侧列表，可稍后手动刷新');
    } else {
      patchTask(t, { status: 'FAILED', msg: String(e.message || e) });
      log('[generate error]', String(e.message || e));
      await send({ cmd: 'song_abort' });
    }
  } finally {
    t.polling = false;
    renderTasks();
  }
}

/* 创建任务 → 提交 → 自动轮询；提交失败直接落 FAILED */
async function startGeneration({ mode, prompt, refAudio, source }) {
  const t = createTask(mode, source);
  try {
    const taskId = await provider.submitGeneration({ mode, prompt, refAudio });
    t.taskId = taskId;
    patchTask(t, { status: 'PENDING', msg: '已入队，等待平台调度' });
    log(`[gen] ${source || mode} 任务已提交 id=${taskId}`);
    await runTaskPolling(t);
  } catch (e) {
    patchTask(t, { status: 'FAILED', msg: String(e.message || e) });
    log('[generate error]', String(e.message || e));
    await send({ cmd: 'song_abort' });
  }
}

/* 手动刷新：单次 query，可反复点击。每次拿最新状态；音频就绪(SUCCESS)即显示"写入开发板"。
 * 不再恢复长时间自动轮询，因此按钮点完一次很快可再点（仅请求在途时短暂禁用）。 */
async function refreshTask(t) {
  if (t.busy || !t.taskId) return;
  t.busy = true;
  renderTasks();
  try {
    const q = await provider.queryTask(t.taskId);
    log('[poll] 手动查询', t.taskId.slice(0, 8), '→', q.status);
    if (q.status === 'SUCCESS') {
      t.song = q.song;
      patchTask(t, { status: 'SUCCESS', msg: `《${q.song.title}》生成完成，点"写入开发板"落盘` });
      log(`[done] 《${q.song.title}》 ${(q.song.size / 1048576).toFixed(2)}MB`);
    } else if (q.status === 'FAILED') {
      patchTask(t, { status: 'FAILED', msg: q.error });
      log('[generate error]', q.error);
    } else {
      // 仍在排队/生成：仅更新到最新状态，保持按钮可点，提示再点一次
      patchTask(t, {
        status: q.status,
        msg: q.status === 'RUNNING'
          ? '平台生成中…（点"刷新状态"再看一次）'
          : '排队中…（点"刷新状态"再看一次）',
      });
    }
  } catch (e) {
    patchTask(t, { msg: `${t.msg}｜单次查询失败：${String(e.message || e)}，可再点刷新` });
    log('[poll] 手动查询失败:', String(e.message || e));
  } finally {
    t.busy = false;
    renderTasks();
  }
}

/* 把某条已完成任务的歌曲写入板载歌曲库（M3 分块协议） */
async function writeTaskToBoard(t) {
  if (!t.song || t.writing) return;
  if (!writer) {
    log('[write] 请先连接开发板再写入');
    return;
  }
  t.writing = true;
  t.msg = '正在写入开发板…';
  renderTasks();
  await send({ cmd: 'gen_state', v: 'start' }); // 写块期间板端跑马灯
  try {
    const bytes = new Uint8Array(await t.song.audioBlob.arrayBuffer());
    await writeSongToBoard(t.song.title, bytes);
    t.written = true;
    t.msg = '已写入开发板，可按 S7 切歌 / 按压旋钮播放';
    await send({ cmd: 'song_list' }); // 落盘后重拉，歌曲库 Tab 即时更新
  } catch (e) {
    log('[write error]', String(e.message || e));
    await send({ cmd: 'song_abort' });
    t.msg = `写板失败：${String(e.message || e)}（可再点写入重试）`;
  } finally {
    await send({ cmd: 'gen_state', v: 'done' });
    t.writing = false;
    renderTasks();
  }
}

/* 任务面板按钮事件委托（刷新状态 / 写入开发板） */
tasksEl.addEventListener('click', e => {
  const btn = e.target.closest('button[data-act]');
  if (!btn) return;
  const t = genTasks.find(x => x.seq === Number(btn.dataset.seq));
  if (!t) return;
  if (btn.dataset.act === 'refresh') refreshTask(t);
  else if (btn.dataset.act === 'write') writeTaskToBoard(t);
});

/* ================= 板载歌曲库 =================
 * 数据来源：板端 song_list 事件（连接时自动同步、写板落盘后重拉、点"刷新"手动重拉）。
 * 仅把 song_list 的 { count, slots, current, songs[] } 渲染到左侧"歌曲库"Tab，
 * 不改动 song_list 原有的日志输出逻辑。 */
let libSongs = [];
let libMeta = { count: 0, slots: 0, current: null };
const libListEl = document.getElementById('libList');
const libCountEl = document.getElementById('libCount');

function renderLibrary() {
  if (!libListEl) return;
  if (libCountEl) {
    libCountEl.textContent = libMeta.slots
      ? `库内 ${libSongs.length}/${libMeta.slots} 首`
      : '未同步';
  }
  if (!libSongs.length) {
    libListEl.innerHTML =
      '<div class="lib-empty"><div class="lib-ico">🎼</div>'
      + '<div class="lib-t">歌曲库为空</div>'
      + '<div class="lib-d">连接开发板后会自动同步板载歌曲；'
      + '在右侧任务卡片点"写入开发板"即可把生成的音乐存入这里。</div></div>';
    return;
  }
  libListEl.innerHTML = libSongs.map(s => {
    const playing = s.slot === libMeta.current;
    const kb = (s.size / 1024).toFixed(0);
    return `<div class="lib-item${playing ? ' playing' : ''}">
      <span class="lib-slot">${esc(s.slot)}</span>
      <span class="lib-main">
        <span class="lib-name" title="${esc(s.title)}">${esc(s.title)}</span>
        <span class="lib-size">${kb} KB</span>
      </span>
      ${playing ? '<span class="lib-now">▶ 当前播放</span>' : ''}
    </div>`;
  }).join('');
}
renderLibrary();

/* "刷新"按钮：向板端重新请求歌曲列表（未连接时 send 内部会打日志并忽略） */
const libRefreshBtn = document.getElementById('libRefresh');
if (libRefreshBtn) {
  libRefreshBtn.addEventListener('click', () => {
    log('[songs] 手动刷新歌曲库…');
    send({ cmd: 'song_list' });
  });
}

/* S1/S2：直接提交文字生成 */
async function runGenerate({ mode, prompt, source }) {
  log(`[generate] mode=${mode} prompt=${prompt ?? '(无)'}`);
  await send({ cmd: 'gen_state', v: 'start' });
  try {
    await startGeneration({ mode, prompt, source });
  } finally {
    await send({ cmd: 'gen_state', v: 'done' });
  }
}

/* S5 哼歌成曲：拉板上录音 → WAV → vocal_to_music（旋律跟随哼唱、保留音色） */
async function runHum() {
  log('[hum_req] 哼歌成曲：先拉取板上录音');
  await send({ cmd: 'gen_state', v: 'start' });
  try {
    const rec = await pullRecording();
    log(`[rec] 拉到 ${(rec.pcm.length / 1024).toFixed(0)}KB @${rec.rate}Hz/${rec.bits}bit/${rec.channels}ch`);
    humDiag(rec.pcm, rec.rate);
    const norm = normalizePcm(rec.pcm);
    if (norm.gain !== 1) log(`[rec·增益] 峰值归一化 ×${norm.gain.toFixed(2)}（改善轻哼，供 vocal_to_music 更好跟随）`);
    const wav = pcmToWav(norm.pcm, rec.rate, rec.bits, rec.channels);
    await startGeneration({ mode: 'reference', refAudio: wav, source: 'S5' });
  } catch (e) {
    log('[hum error]', String(e.message || e));
    await send({ cmd: 'song_abort' });
  } finally {
    await send({ cmd: 'gen_state', v: 'done' });
  }
}

/* 哼唱信号诊断：量时长/峰值/RMS，判断拉到的录音是否是"健康人声"
 * （太短/太轻/削顶都会让 vocal_to_music 基本忽略哼唱、按 caption 自由发挥） */
function humDiag(pcm, rate) {
  const n = Math.floor(pcm.length / 2);
  if (n === 0) { log('[rec·诊断] ⚠ 空录音（0 帧），板上可能没录到声音'); return; }
  const view = new DataView(pcm.buffer, pcm.byteOffset, pcm.byteLength);
  let peak = 0, sumSq = 0;
  for (let i = 0; i < n; i++) {
    const s = view.getInt16(i * 2, true); // 16bit 小端
    const a = s < 0 ? -s : s;
    if (a > peak) peak = a;
    sumSq += s * s;
  }
  const rms = Math.round(Math.sqrt(sumSq / n));
  const sec = n / rate;
  const full = 32768;
  const rmsPct = (rms / full * 100).toFixed(2);
  log(`[rec·诊断] 时长 ${sec.toFixed(2)}s，峰值 ${peak}/${full}(${(peak / full * 100).toFixed(0)}%)，RMS ${rms}(${rmsPct}%)`);
  const flags = [];
  if (sec < 1.2) flags.push('太短（≥2s 哼一段更稳）');
  if (rms < full * 0.02) flags.push('音量太低/近乎静音（离麦近一点或哼响一点）');
  else if (rms > full * 0.35) flags.push('可能过响削顶');
  if (peak >= full - 1) flags.push('有削顶(clipping)');
  log(flags.length
    ? `[rec·诊断] ⚠ ${flags.join('；')}——这会让哼歌成曲偏离原哼唱`
    : '[rec·诊断] ✓ 信号健康，哼唱内容应能被 vocal_to_music 采纳');
}

/* 峰值归一化：把偏轻的哼唱整体抬到接近满量程，改善 vocal_to_music 的旋律跟随。
 * 只升不降（gain≥1），并封顶 4× 以免把纯静音/底噪放大成噪声。返回 {pcm, gain}。 */
function normalizePcm(pcm) {
  const n = Math.floor(pcm.length / 2);
  if (n === 0) return { pcm, gain: 1 };
  const src = new DataView(pcm.buffer, pcm.byteOffset, pcm.byteLength);
  let peak = 0;
  for (let i = 0; i < n; i++) {
    const a = Math.abs(src.getInt16(i * 2, true));
    if (a > peak) peak = a;
  }
  const target = Math.round(32768 * 0.92);
  if (peak === 0 || peak >= target) return { pcm, gain: 1 }; // 已有电平或空，不动
  let gain = target / peak;
  if (gain > 4) gain = 4; // 封顶，避免放大底噪
  const out = new Uint8Array(pcm.length);
  const dst = new DataView(out.buffer);
  for (let i = 0; i < n; i++) {
    let s = src.getInt16(i * 2, true) * gain;
    if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
    dst.setInt16(i * 2, s, true);
  }
  return { pcm: out, gain };
}

/* S6 录音转文字生成：拉录音 → WAV → ASR 转文字 → 文字作 caption，带词演唱生成歌曲
 * （歌词由平台按 caption 自动撰写并演唱，非纯音乐） */
function pickAsrSource() {
  const cloud = document.getElementById('asrSrcCloud');
  return cloud && cloud.checked ? 'cloud' : 'local';
}

async function runPrompt() {
  log('[prompt_req] 录音转文字生成：先拉取板上录音');
  await send({ cmd: 'gen_state', v: 'start' });
  try {
    const rec = await pullRecording();
    log(`[rec] 拉到 ${(rec.pcm.length / 1024).toFixed(0)}KB @${rec.rate}Hz`);
    const wav = pcmToWav(rec.pcm, rec.rate, rec.bits, rec.channels);

    const src = pickAsrSource();
    log(`[asr] 转写中…（来源：${src === 'local' ? '本地 SenseVoice' : '阶跃云端'}）`);
    let text;
    try {
      text = src === 'local' ? await provider.transcribeLocal(wav) : await provider.transcribe(wav);
    } catch (e) {
      if (src === 'local') {
        log('[asr]', String(e.message || e));
        throw new Error('本地识别失败：' + (e.message || e) + '；可在顶栏切「阶跃云端」后重按 S6');
      }
      throw e;
    }
    log('[asr]', text || '(空)');
    if (!text.trim()) throw new Error('录音里没有识别到文字，请靠近麦克风清晰说一句再试');
    await startGeneration({ mode: 'song', prompt: text, source: 'S6' });
  } catch (e) {
    log('[prompt error]', String(e.message || e));
    await send({ cmd: 'song_abort' });
  } finally {
    await send({ cmd: 'gen_state', v: 'done' });
  }
}

/* 板端事件分发 */
async function handleEvent(ev) {
  serveWaiters(ev);
  switch (ev.cmd || ev.t) {
    case 'hello':
      log('[hello]', ev);
      await send({ cmd: 'song_list' }); // 连接建立后同步歌曲库
      break;
    case 'song_list':
      log(`[songs] 库内 ${ev.count}/${ev.slots} 首，当前=${ev.current}`);
      (ev.songs || []).forEach(s =>
        log(`  [${s.slot}] 《${s.title}》 ${(s.size / 1024).toFixed(0)}KB`),
      );
      libSongs = (ev.songs || []).map(s => ({
        slot: s.slot, title: s.title, size: s.size || 0,
      }));
      libMeta = { count: ev.count ?? libSongs.length, slots: ev.slots ?? 0,
                  current: ev.current ?? null };
      renderLibrary();
      break;
    case 'gen_req':
      // S1 纯音乐 / S2 带歌词
      log('[gen_req]', ev.type);
      // 不 await：runGenerate 内部要等板端 ACK，收流循环必须继续跑
      runGenerate({
        mode: ev.type,
        source: ev.type === 'instrumental' ? 'S1' : 'S2',
        prompt: pick(PROMPT_POOLS[ev.type] || PROMPT_POOLS.song),
      });
      break;
    case 'hum_req':
      // S5 哼歌成曲：拉板上录音 → vocal_to_music
      log('[hum_req]');
      runHum();
      break;
    case 'prompt_req':
      // S6：拉板上录音 → ASR 转文字作 caption → 带词演唱（歌词自动写）
      log('[prompt_req]');
      runPrompt();
      break;
    default:
      // ack/err/ev 多已被等待器消费，不再重复打印
      if (!('ack' in ev) && !('err' in ev) && !('ev' in ev)) log('[rx]', ev);
  }
}

/* ================= M4 录音回传：接收侧解析 =================
 * CDC 是无边界字节流，同一 chunk 里可能"JSON 行 + 二进制帧"混合，
 * 故用字节级双模状态机：text 模式按 \n 拼行；收到 rec_meta 后切 frame 模式
 * 解析与 M3 同款的二进制帧；帧齐（got==size）再切回 text 读 rec_end。 */

const REC_TIMEOUT_MS = 60000;
const REC_ERR_HINT = {
  no_recording: '板上没有录音，请先在板上录一段',
  busy: '板端忙（正在写歌或录音中），稍后再试',
  io: '板端读取录音失败',
};

let recPull = null; // 进行中的拉取会话；无则 null
let rxMode = 'text'; // 'text' 收 JSON 行 | 'frame' 收二进制帧
let textBuf = []; // 文本行累积字节（UTF-8，行尾再解码）
const pendingEvents = []; // 已解析待分发事件（保持 handleEvent 顺序）

/* 帧解析子状态机（与 buildFrame 对称） */
const FP = { SYNC0: 0, SYNC1: 1, HDR: 2, PAY: 3, CRC: 4 };
let fp = null;
let fpSkip = 0; // SYNC0 态跳过的非帧字节数（诊断：区分"没收到字节"与"收到的不是帧"）

function fpReset() {
  fp = { st: FP.SYNC0, hdr: [0, 0, 0, 0], hdrIdx: 0, seq: 0, len: 0,
         payload: null, payIdx: 0, crc: 0, crcIdx: 0 };
}
fpReset();

function rxFrameByte(b) {
  switch (fp.st) {
    case FP.SYNC0:
      if (b === 0xaa) { fp.st = FP.SYNC1; fpSkip = 0; }
      else if (++fpSkip === 512) log('[rec] 警告：已跳过 512 个非帧字节，收到的可能不是帧数据');
      break;
    case FP.SYNC1:
      if (b === 0x55) { fp.hdrIdx = 0; fp.st = FP.HDR; }
      else if (b !== 0xaa) fp.st = FP.SYNC0;
      break;
    case FP.HDR:
      fp.hdr[fp.hdrIdx++] = b;
      if (fp.hdrIdx === 4) {
        fp.seq = fp.hdr[0] | (fp.hdr[1] << 8);
        fp.len = fp.hdr[2] | (fp.hdr[3] << 8);
        if (fp.len === 0 || fp.len > CHUNK) { failPull(`录音帧长度非法（${fp.len}）`); return; }
        fp.payload = new Uint8Array(fp.len);
        fp.payIdx = 0;
        fp.st = FP.PAY;
      }
      break;
    case FP.PAY:
      fp.payload[fp.payIdx++] = b;
      if (fp.payIdx === fp.len) { fp.crc = 0; fp.crcIdx = 0; fp.st = FP.CRC; }
      break;
    case FP.CRC:
      fp.crc |= b << (8 * fp.crcIdx);
      fp.crcIdx++;
      if (fp.crcIdx === 4) {
        const seq = fp.seq, payload = fp.payload, want = fp.crc >>> 0;
        if (crc32(payload) !== want) { failPull(`录音帧 ${seq} CRC 校验失败`); return; }
        fpReset(); // 校验通过再复位，失败时保留现场供诊断
        onRecFrame(payload);
      }
      break;
  }
}

function rxTextByte(b) {
  if (b === 0x0a || b === 0x0d) {
    if (textBuf.length) {
      const s = new TextDecoder().decode(Uint8Array.from(textBuf));
      textBuf = [];
      dispatchLine(s);
    }
    return;
  }
  if (textBuf.length < 8192) textBuf.push(b);
}

function dispatchLine(line) {
  const s = line.trim();
  if (!s) return;
  if (!s.startsWith('{')) { log('[fw]', s); return; }
  let ev;
  try { ev = JSON.parse(s); } catch { log('[rx·bad json]', s); return; }
  if (ev.ev === 'rec_meta') { onRecMeta(ev); return; }
  if (ev.ev === 'rec_end') { onRecEnd(ev); return; }
  if (ev.err === 'rec_pull') { failPull(REC_ERR_HINT[ev.msg] || ev.msg || 'rec_pull 失败'); return; }
  pendingEvents.push(ev); // 其余走通用分发（handleEvent）
}

function onRecMeta(ev) {
  if (!recPull) { log('[rec] 收到未请求的 rec_meta，忽略'); return; }
  recPull.size = ev.size >>> 0;
  recPull.rate = ev.rate || 16000;
  recPull.bits = ev.bits || 16;
  recPull.channels = ev.channels || 1;
  recPull.got = 0;
  recPull.chunks = [];
  log(`[rec] meta ${recPull.size} 字节 @${recPull.rate}Hz/${recPull.bits}bit/${recPull.channels}ch`);
  fpReset();
  rxMode = 'frame';
}

function onRecFrame(payload) {
  if (!recPull) return;
  recPull.chunks.push(payload);
  recPull.got += payload.length;
  recPull.frames = (recPull.frames || 0) + 1;
  if (recPull.frames % 16 === 0 || recPull.got >= recPull.size) {
    log(`[rec] 进度 ${recPull.got}/${recPull.size}（${recPull.frames} 帧）`);
  }
  if (recPull.got >= recPull.size) rxMode = 'text'; // 帧齐，后续是 rec_end 文本行
}

function onRecEnd(ev) {
  const p = recPull;
  if (!p) return;
  clearTimeout(p.timer);
  recPull = null;
  if (p.got !== p.size) { p.reject(new Error(`录音数据不完整：${p.got}/${p.size}`)); return; }
  const pcm = new Uint8Array(p.got);
  let off = 0;
  for (const c of p.chunks) { pcm.set(c, off); off += c.length; }
  if (crc32(pcm) !== (ev.crc >>> 0)) { p.reject(new Error('整段录音 CRC 校验失败')); return; }
  p.resolve({ pcm, rate: p.rate, bits: p.bits, channels: p.channels });
}

/* 拉取失败/中止：复位解析模式并 reject 进行中的会话 */
function failPull(reason) {
  // 先捕获现场，再复位（否则 fpReset 后 st 恒为 0、payIdx 恒为 0，诊断失去意义）
  const diag = `（rxMode=${rxMode} 帧解析 st=${fp.st} seq=${fp.seq || 0} len=${fp.len || 0} `
    + `已收帧内 ${fp.payIdx || 0}B 跳过非帧 ${fpSkip}B）`;
  rxMode = 'text';
  textBuf = [];
  fpReset();
  fpSkip = 0;
  const p = recPull;
  if (!p) { log('[rec] 失败:', reason, diag); return; }
  clearTimeout(p.timer);
  recPull = null;
  p.reject(new Error(reason + diag));
}

/* 请求板上最近一段录音，返回 { pcm, rate, bits, channels } */
function pullRecording() {
  if (recPull) return Promise.reject(new Error('已有录音拉取进行中'));
  return new Promise((resolve, reject) => {
    recPull = { resolve, reject, timer: null, size: 0, rate: 16000, bits: 16,
                channels: 1, got: 0, chunks: [], frames: 0 };
    recPull.timer = setTimeout(() => failPull('拉取录音超时'), REC_TIMEOUT_MS);
    log('[rec] 请求板上最近录音…');
    send({ cmd: 'rec_pull' });
  });
}

/* PCM(16bit/mono) → WAV Blob：阶跃接口只接受容器格式 */
function pcmToWav(pcm, rate, bits, channels) {
  const blockAlign = (bits / 8) * channels;
  const byteRate = rate * blockAlign;
  const dataSize = pcm.length;
  const buf = new ArrayBuffer(44 + dataSize);
  const v = new DataView(buf);
  const wstr = (off, s) => { for (let i = 0; i < s.length; i++) v.setUint8(off + i, s.charCodeAt(i)); };
  wstr(0, 'RIFF'); v.setUint32(4, 36 + dataSize, true); wstr(8, 'WAVE');
  wstr(12, 'fmt '); v.setUint32(16, 16, true); v.setUint16(20, 1, true); // PCM
  v.setUint16(22, channels, true); v.setUint32(24, rate, true);
  v.setUint32(28, byteRate, true); v.setUint16(32, blockAlign, true);
  v.setUint16(34, bits, true);
  wstr(36, 'data'); v.setUint32(40, dataSize, true);
  new Uint8Array(buf, 44).set(pcm);
  return new Blob([buf], { type: 'audio/wav' });
}

/* 逐字节喂入状态机 */
function feedBytes(bytes) {
  for (let i = 0; i < bytes.length; i++) {
    if (rxMode === 'frame') rxFrameByte(bytes[i]);
    else rxTextByte(bytes[i]);
  }
}

/* 收流：字节级双模解析（文本 JSON 行 / M4 二进制帧） */
async function readLoop() {
  while (port && port.readable) {
    const reader = port.readable.getReader();
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        if (value && value.length) feedBytes(value);
        while (pendingEvents.length) await handleEvent(pendingEvents.shift());
      }
    } catch (e) {
      log('[rx error]', String(e));
    } finally {
      reader.releaseLock();
    }
  }
}

async function connect() {
  if (!('serial' in navigator)) {
    log('此浏览器不支持 Web Serial（请用 Chrome/Edge）');
    return;
  }
  try {
    port = await navigator.serial.requestPort();
    await port.open({ baudRate: BAUD });
    writer = port.writable.getWriter();
    statusEl.textContent = 'connected';
    connectBtn.textContent = 'Disconnect';
    log('connected @', BAUD);
    await send({ cmd: 'ping' });
    readLoop();
  } catch (e) {
    log('[connect error]', String(e));
    port = null;
    writer = null;
  }
}

async function disconnect() {
  try {
    rejectAllWaiters('串口已断开');
    failPull('串口已断开'); // 中止进行中的录音拉取
    if (writer) { writer.releaseLock(); writer = null; }
    if (port) { await port.close(); }
  } catch { /* 忽略关闭异常 */ }
  port = null;
  statusEl.textContent = 'disconnected';
  connectBtn.textContent = 'Connect USB';
  log('disconnected');
}

connectBtn.addEventListener('click', () => (port ? disconnect() : connect()));

/* ================= 音乐生成控制台（板无关·手动全参数） =================
 * 直连阶跃 provider.submitRaw / queryTaskRaw，逐个字段透传 API 参数，
 * 便于对比各类生成效果、手动触发状态查询、成功后直接播放/下载/写板。
 * 参考音频来自本地文件选择器（运行时选，不打包进应用）。 */
const CON_MIME = {
  mp3: 'audio/mpeg', wav: 'audio/wav', flac: 'audio/flac',
  opus: 'audio/ogg', pcm: 'audio/wav', // pcm 裸流浏览器多半无法直接播，仍可下载
};
const conEl = {
  task: document.getElementById('conTask'),
  model: document.getElementById('conModel'),
  instrumental: document.getElementById('conInstrumental'),
  caption: document.getElementById('conCaption'),
  lyrics: document.getElementById('conLyrics'),
  vocalFile: document.getElementById('conVocalFile'),
  vocalInfo: document.getElementById('conVocalInfo'),
  songFile: document.getElementById('conSongFile'),
  songInfo: document.getElementById('conSongInfo'),
  format: document.getElementById('conFormat'),
  sampleRate: document.getElementById('conSampleRate'),
  bitRate: document.getElementById('conBitRate'),
  temperature: document.getElementById('conTemperature'),
  topK: document.getElementById('conTopK'),
  topP: document.getElementById('conTopP'),
  repPenalty: document.getElementById('conRepPenalty'),
  maxTokens: document.getElementById('conMaxTokens'),
  submit: document.getElementById('conSubmit'),
  query: document.getElementById('conQuery'),
  taskId: document.getElementById('conTaskId'),
  result: document.getElementById('conResult'),
};
const conState = {
  taskId: null, submitting: false, querying: false, writing: false,
  vocalFile: null, songFile: null, audioUrl: null, audioBlob: null, audioTitle: '',
};

function conReadNum(input) {
  const s = (input.value || '').trim();
  if (s === '') return undefined;
  const n = Number(s);
  return Number.isFinite(n) ? n : undefined;
}

function conDropAudio() {
  if (conState.audioUrl) { URL.revokeObjectURL(conState.audioUrl); conState.audioUrl = null; }
  conState.audioBlob = null;
}

function conResetResult(html) {
  conEl.result.innerHTML = html;
}

// 文件选择：暂存 File + 显示大小
function conBindFile(input, infoEl, key) {
  if (!input) return;
  input.addEventListener('change', () => {
    const f = input.files && input.files[0];
    conState[key] = f || null;
    infoEl.textContent = f
      ? `已选：${f.name}（${(f.size / 1024).toFixed(0)}KB）`
      : '';
  });
}
conBindFile(conEl.vocalFile, conEl.vocalInfo, 'vocalFile');
conBindFile(conEl.songFile, conEl.songInfo, 'songFile');

// 由表单构建阶跃请求体；校验失败抛 Error（调用方捕获）
function conBuildParams() {
  const task = conEl.task.value;
  const model_id = (conEl.model.value || '').trim();
  const caption = (conEl.caption.value || '').trim();
  const lyrics = (conEl.lyrics.value || '').trim();
  const instrumental = conEl.instrumental.checked;

  if (!model_id) throw new Error('model_id 必填');
  if (!caption) throw new Error('caption 必填（风格描述）');
  if (instrumental && task !== 'text_to_music') {
    throw new Error('instrumental 仅 text_to_music 支持');
  }
  if ((task === 'vocal_to_music' || task === 'music_cover') && !lyrics) {
    throw new Error(`${task} 需要 lyrics（干声内容应是歌词子集/全集）`);
  }
  if (task === 'vocal_to_music' && !conState.vocalFile) {
    throw new Error('vocal_to_music 需要选择 vocal_audio 干声文件');
  }
  if (task === 'music_cover' && !conState.songFile) {
    throw new Error('music_cover 需要选择 song_audio 参考歌曲文件');
  }

  const p = { model_id, task, caption, response_format: conEl.format.value };
  if (instrumental) p.instrumental = true;
  if (!instrumental && lyrics) p.lyrics = lyrics;

  // 可选数值：留空不传，用平台默认
  const nums = {
    sample_rate: conReadNum(conEl.sampleRate),
    temperature: conReadNum(conEl.temperature),
    top_k: conReadNum(conEl.topK),
    top_p: conReadNum(conEl.topP),
    repetition_penalty: conReadNum(conEl.repPenalty),
    max_tokens: conReadNum(conEl.maxTokens),
  };
  for (const k in nums) if (nums[k] !== undefined) p[k] = nums[k];
  const br = (conEl.bitRate.value || '').trim();
  if (br) p.bit_rate = Number.isFinite(Number(br)) ? Number(br) : br;

  return p;
}

async function conSubmit() {
  if (conState.submitting || conState.querying) return;
  let params;
  try {
    params = conBuildParams();
  } catch (e) {
    conDropAudio();
    conResetResult(`<span class="muted" style="color:#ffb3c4">✗ ${esc(String(e.message || e))}</span>`);
    return;
  }
  conState.submitting = true;
  conEl.submit.disabled = true;
  conEl.submit.textContent = '提交中…';
  try {
    conDropAudio(); // 新提交会替换结果区，先释放上一段可播放音频
    if (conState.vocalFile) params.vocal_audio = await provider.blobToBase64(conState.vocalFile);
    if (conState.songFile) params.song_audio = await provider.blobToBase64(conState.songFile);

    const shown = { ...params };
    if (shown.vocal_audio) shown.vocal_audio = `[base64 ${Math.round(shown.vocal_audio.length / 1024)}KB]`;
    if (shown.song_audio) shown.song_audio = `[base64 ${Math.round(shown.song_audio.length / 1024)}KB]`;
    log('[console·submit]', shown);

    const data = await provider.submitRaw(params);
    if (!data || !data.task_id) throw new Error('响应缺少 task_id：' + JSON.stringify(data));
    conState.taskId = data.task_id;
    conEl.taskId.textContent = 'task_id ' + data.task_id.slice(0, 8);
    conEl.query.disabled = false;
    conResetResult(
      `<div class="cf-status">已提交，排队中<span class="tbadge">${STATUS_BADGE.PENDING}</span></div>`
      + `<span class="muted">点"查询状态"拉取最新进展（任务 ${esc(data.task_id.slice(0, 12))}…）。</span>`
    );
    log('[console] 提交成功 task_id=' + data.task_id);
  } catch (e) {
    const msg = String(e.message || e);
    conResetResult(`<span class="muted" style="color:#ffb3c4">✗ 提交失败：${esc(msg)}</span>`);
    log('[console error]', msg);
  } finally {
    conState.submitting = false;
    conEl.submit.disabled = false;
    conEl.submit.textContent = '提交生成';
  }
}

async function conQuery() {
  if (!conState.taskId || conState.querying || conState.submitting) return;
  conState.querying = true;
  conEl.query.disabled = true;
  conEl.query.textContent = '查询中…';
  try {
    const data = await provider.queryTaskRaw(conState.taskId);
    conRenderQuery(data);
  } catch (e) {
    conDropAudio();
    conResetResult(`<span class="muted" style="color:#ffb3c4">✗ 查询失败：${esc(String(e.message || e))}</span>`);
    log('[console error] 查询失败:', String(e.message || e));
  } finally {
    conState.querying = false;
    conEl.query.disabled = !conState.taskId;
    conEl.query.textContent = '查询状态';
  }
}

function conRenderQuery(data) {
  const status = data.status || 'UNKNOWN';
  const badge = STATUS_BADGE[status] || status;
  // 非"成功且有音频"的分支：清掉上一段可播放结果（成功分支内部自行先 revoke 再建新 URL）
  if (!(status === 'SUCCESS' && data.audio)) conDropAudio();
  const rawSafe = { ...data };
  if (rawSafe.audio) rawSafe.audio = `[base64 ${Math.round(String(rawSafe.audio).length / 1024)}KB]`;
  let body = `<div class="cf-status">状态：<span class="tbadge">${esc(badge)}</span> ${esc(status)}</div>`;

  if (status === 'SUCCESS' && data.audio) {
    const fmt = conEl.format.value || 'mp3';
    const blob = conB64ToBlob(data.audio, CON_MIME[fmt] || 'application/octet-stream');
    if (conState.audioUrl) URL.revokeObjectURL(conState.audioUrl);
    conState.audioBlob = blob;
    conState.audioUrl = URL.createObjectURL(blob);
    conState.audioTitle = data.rewritten_caption || data.caption || `控制台生成 ${conState.taskId.slice(0, 6)}`;
    const sizeK = (blob.size / 1048576).toFixed(2);
    body +=
      `<audio controls src="${conState.audioUrl}"></audio>`
      + `<div class="cf-audio-row">`
      + `<a class="tlink" download="${esc(conState.audioTitle + '.' + fmt)}" href="${conState.audioUrl}">下载 ${fmt}（${sizeK}MB）</a>`
      + `<button data-cact="write" ${conState.writing ? 'disabled' : ''}>${conState.writing ? '写入中…' : '写入开发板'}</button>`
      + (fmt === 'pcm' ? '<span class="muted">（pcm 裸流可能无法直接播放，请下载后解析）</span>' : '')
      + `</div>`;
    if (data.rewritten_lyrics || data.lyrics) {
      const ly = String(data.rewritten_lyrics || data.lyrics);
      body += `<span class="muted">歌词：${esc(ly.slice(0, 160))}${ly.length > 160 ? '…' : ''}</span>`;
    }
    log('[console] SUCCESS 音频就绪', sizeK + 'MB', fmt);
  } else if (status === 'SUCCESS') {
    body += `<span class="muted" style="color:#ffb3c4">SUCCESS 但响应缺少 audio 字段（结果可能已过期），请重新提交。</span>`;
  } else if (status === 'FAILED') {
    const stage = data.error?.stage || 'unknown';
    body += `<span class="muted" style="color:#ffb3c4">✗ 失败 [${esc(stage)}]：${esc(data.error?.message || '')}</span>`;
    log('[console] FAILED', stage, data.error?.message || '');
  } else {
    body += `<span class="muted">仍在排队/生成中，稍后再点"查询状态"。</span>`;
  }
  body += `<pre class="cf-raw">${esc(JSON.stringify(rawSafe, null, 2))}</pre>`;
  conResetResult(body);
}

function conB64ToBlob(b64, mime) {
  const bin = atob(b64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return new Blob([bytes], { type: mime });
}

async function conWrite() {
  if (!conState.audioBlob || conState.writing) return;
  if (!writer) { log('[console] 请先连接开发板再写入'); return; }
  conState.writing = true;
  conEl.result.querySelector('[data-cact="write"]')?.setAttribute('disabled', '');
  await send({ cmd: 'gen_state', v: 'start' });
  try {
    const bytes = new Uint8Array(await conState.audioBlob.arrayBuffer());
    await writeSongToBoard(conState.audioTitle, bytes);
    await send({ cmd: 'song_list' });
    log('[console] 已写入开发板：', conState.audioTitle);
  } catch (e) {
    log('[console write error]', String(e.message || e));
    await send({ cmd: 'song_abort' });
  } finally {
    conState.writing = false;
    conEl.result.querySelector('[data-cact="write"]')?.removeAttribute('disabled');
    await send({ cmd: 'gen_state', v: 'done' });
  }
}

conEl.submit.addEventListener('click', conSubmit);
conEl.query.addEventListener('click', conQuery);
conEl.result.addEventListener('click', e => {
  const b = e.target.closest('[data-cact="write"]');
  if (b) conWrite();
});
