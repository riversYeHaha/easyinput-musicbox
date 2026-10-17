/*
 * Mock Provider：开发期占位，不阻塞固件与链路开发。
 * 统一接口：generate({ mode: "instrumental" | "song", prompt?, refAudio? }) → audioURL
 */
export async function generate({ mode, prompt, refAudio } = {}) {
  console.log('[mock provider] generate()', { mode, prompt, refAudio });
  // TODO(M3)：返回内置测试音频 URL
  return Promise.resolve(null);
}
