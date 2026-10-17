import { defineConfig } from 'vite';
import os from 'node:os';
import path from 'node:path';
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import { spawn } from 'node:child_process';

/*
 * 开发代理：浏览器直连 api.stepfun.com 会跨域，
 * dev 服务器把 /stepfun/* 转发到阶跃开放平台。
 * 生产部署如需同源反代，可用 VITE_STEPFUN_BASE 覆盖 provider 里的 BASE。
 */

/*
 * 本地 SenseVoice ASR 桥接（S6 录音转文字）
 * ------------------------------------------------------------------
 * 浏览器无法直接调用命令行，这里在 Vite dev server 内挂一个同源
 * POST /localasr/transcribe 中间件：收 WAV 原始字节 → 写临时文件 →
 * 调 AniShort 下载的 funasr CLI（SenseVoiceSmall，本地离线、不吃云端额度）
 * → 解析 stdout 文本 → 返回 { text }。
 *
 * 关键：funasr shim 用 AniShort 内嵌 Python 的 shebang 运行，但 userbase 的
 * site-packages 不在默认 sys.path，直接执行会 ModuleNotFoundError: funasr。
 * 因此显式注入 PYTHONPATH，并把 MODELSCOPE_CACHE 指到已下载模型目录避免重下。
 * 实测：16kHz 单声道 WAV 与板端录音参数完全一致，无需重采样。
 *
 * 缺失环境时中间件返回 501 并给出清晰提示，应用其余功能不受影响（可切云端）。
 * 可用环境变量覆盖：MUSICBOX_FUNASR_BIN / MUSICBOX_MODELSCOPE_CACHE。
 */

const HOME = os.homedir();

/* 探测 AniShort 的 python-userbase，兼容任意 python3.x 小版本 */
function detectAsrEnv() {
  const env = {
    funasrBin:
      process.env.MUSICBOX_FUNASR_BIN ||
      path.join(HOME, '.anishort/python-userbase/bin/funasr'),
    modelscopeCache:
      process.env.MUSICBOX_MODELSCOPE_CACHE ||
      path.join(HOME, '.anishort/models/modelscope'),
    sitePackages: null,
  };

  const userbase = path.join(HOME, '.anishort/python-userbase');
  try {
    const libDir = path.join(userbase, 'lib');
    if (fs.existsSync(libDir)) {
      const py = fs
        .readdirSync(libDir)
        .filter((d) => d.startsWith('python3'))
        .sort()
        .reverse()
        .find((d) => fs.existsSync(path.join(libDir, d, 'site-packages', 'funasr')));
      if (py) env.sitePackages = path.join(libDir, py, 'site-packages');
    }
  } catch {
    /* ignore */
  }

  env.ready =
    fs.existsSync(env.funasrBin) &&
    !!env.sitePackages &&
    fs.existsSync(env.sitePackages);
  return env;
}

const ASR = detectAsrEnv();

/* 顺序队列：SenseVoice 冷启加载 ~1GB 权重，避免并发重复加载打爆内存 */
let asrChain = Promise.resolve();

/* 运行一次 funasr CLI，返回转写文本 */
function runFunasr(wavPath, language) {
  return new Promise((resolve, reject) => {
    const env = {
      ...process.env,
      PYTHONPATH: ASR.sitePackages,
      MODELSCOPE_CACHE: ASR.modelscopeCache,
      PYTHONUNBUFFERED: '1',
    };
    const args = [
      '--model', 'sensevoice',
      '-l', language || 'auto',
      '-f', 'text',
      '--device', 'cpu',
      wavPath,
    ];

    const child = spawn(ASR.funasrBin, args, { env });
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (d) => (stdout += d.toString('utf8')));
    child.stderr.on('data', (d) => (stderr += d.toString('utf8')));
    child.on('error', reject);
    child.on('close', (code) => {
      // tqdm 进度与 log 走 stderr；stdout 兜底清洗一遍防混入
      const text = stdout
        .split('\n')
        .map((l) => l.replace(/\r/g, '').trim())
        .filter(
          (l) =>
            l.length > 0 &&
            !/^\d{4}-\d{2}-\d{2} /.test(l) &&
            !/it\/s\]?\s*$/.test(l) &&
            !/^rtf/.test(l) &&
            !/^\{'/.test(l) &&
            !/funasr version/i.test(l) &&
            !/version:/i.test(l) &&
            !/downloading/i.test(l),
        )
        .join('\n')
        .trim();
      if (!text) {
        reject(new Error(`本地 ASR 无输出（退出码 ${code}）：\n${stderr.slice(-800)}`));
      } else {
        resolve(text);
      }
    });
  });
}

function localAsrBridge() {
  return {
    name: 'local-asr-bridge',
    configureServer(server) {
      server.middlewares.use('/localasr/transcribe', async (req, res) => {
        res.setHeader('Content-Type', 'application/json; charset=utf-8');
        const send = (status, obj) => {
          res.statusCode = status;
          res.end(JSON.stringify(obj));
        };

        if (req.method !== 'POST') return send(405, { error: '只支持 POST' });
        if (!ASR.ready) {
          return send(501, {
            error:
              '本机未探测到 AniShort 本地 ASR 环境（funasr CLI / SenseVoice 模型）。' +
              '请确认 AniShort 已下载 ASR 模型，或在界面把识别来源切到「阶跃云端」。',
          });
        }

        const chunks = [];
        for await (const c of req) chunks.push(c);
        const body = Buffer.concat(chunks);
        if (body.length < 44) return send(400, { error: '音频数据为空或过小' });

        const lang = (req.headers['x-asr-language'] || 'auto').toString();
        const tmp = path.join(
          os.tmpdir(),
          `musicbox-asr-${Date.now()}-${process.pid}.wav`,
        );
        try {
          await fsp.writeFile(tmp, body);
          const task = asrChain.then(() => runFunasr(tmp, lang));
          asrChain = task.catch(() => {});
          const text = await task;
          send(200, { text });
        } catch (e) {
          send(500, { error: String(e && e.message ? e.message : e) });
        } finally {
          fsp.unlink(tmp).catch(() => {});
        }
      });

      server.httpServer?.once('listening', () => {
        server.config.logger.info(
          ASR.ready
            ? `\x1b[36m  ➜  本地 ASR:  POST /localasr/transcribe (SenseVoice ✓)\x1b[0m`
            : `\x1b[33m  ➜  本地 ASR: 不可用（未探测到 funasr/模型），S6 可切阶跃云端\x1b[0m`,
        );
      });
    },
  };
}

export default defineConfig({
  plugins: [localAsrBridge()],
  server: {
    proxy: {
      '/stepfun': {
        target: 'https://api.stepfun.com',
        changeOrigin: true,
        rewrite: (p) => p.replace(/^\/stepfun/, ''),
      },
    },
  },
});
