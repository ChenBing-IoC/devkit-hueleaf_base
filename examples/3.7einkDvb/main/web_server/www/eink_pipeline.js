/* Six-ink perceptual renderer and the OTP full-frame transport. */
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.EinkPipeline = api;
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';
  const WIDTH = 480, HEIGHT = 720, FRAME_BYTES = WIDTH * HEIGHT / 2;
  const FORMAT = 'otp6-v1';
  let panelRotation = 0, rotationEpoch = 1;
  function setPanelRotation(degrees, epoch = rotationEpoch) {
    if (![0,90,180,270].includes(degrees)) throw new Error('不支持的整屏安装方向');
    panelRotation = degrees; rotationEpoch = epoch;
  }
  function sourceIndex(x, y) {
    return panelRotation === 90 ? (WIDTH-1-x)*HEIGHT+y : panelRotation === 270 ? x*HEIGHT+HEIGHT-1-y : panelRotation === 180 ? (HEIGHT - 1 - y) * WIDTH + WIDTH - 1 - x : y * WIDTH + x;
  }

  const COLORS = Object.freeze([
    { id: 0, name: '黑', css: '#111412' },
    { id: 1, name: '白', css: '#f4f3e8' },
    { id: 2, name: '黄', css: '#e7c83f' },
    { id: 3, name: '红', css: '#c63b35' },
    { id: 5, name: '蓝', css: '#315eae' },
    { id: 6, name: '绿', css: '#398451' },
  ].map(Object.freeze));
  const clamp = (x, lo, hi) => Math.max(lo, Math.min(hi, x));
  const linear = Float32Array.from({ length: 256 }, (_, i) => {
    const v = i / 255;
    return v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4;
  });
  // OKLab is used for ink selection, error diffusion and tone mapping. sRGB
  // distances (and even linear-RGB ones) give visibly wrong hues on a six-ink
  // panel: blue/green mixtures win over the perceptually closer ink, which is
  // what makes the old renderer look muddy and colour-speckled.
  function oklabFromLinear(r, g, b) {
    const l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b;
    const m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b;
    const s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b;
    const l_ = Math.cbrt(l), m_ = Math.cbrt(m), s_ = Math.cbrt(s);
    return [
      0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
      1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
      0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_,
    ];
  }
  const palette = COLORS.map(c => {
    const rgb = [1, 3, 5].map(i => parseInt(c.css.slice(i, i + 2), 16));
    const lin = rgb.map(v => linear[v]);
    return { ...c, rgb, lin, lab: oklabFromLinear(lin[0], lin[1], lin[2]) };
  });
  const validInk = id => Number.isInteger(id) && (id === 0 || id === 1 || id === 2 || id === 3 || id === 5 || id === 6);
  // Diffusion strength. Full strength keeps the mean colour exact; lowering it
  // trades a little accuracy for a calmer, less noisy texture.
  const MODES = Object.freeze({ fast: .6, standard: .8, best: .95 });
  const canceledError = () => Object.assign(new Error('预览已更新'), { name: 'AbortError' });

  function logicalSize(){return panelRotation%180?{width:720,height:480}:{width:480,height:720};}

  // sRGB -> linear light -> OKLab, with alpha composited over paper white.
  function sourceLab(rgba, width, height) {
    const lab = new Float32Array(width * height * 3);
    for (let i = 0, p = 0; i < width * height; i++, p += 4) {
      const a = rgba[p + 3] / 255;
      const r = linear[rgba[p]] * a + (1 - a);
      const g = linear[rgba[p + 1]] * a + (1 - a);
      const b = linear[rgba[p + 2]] * a + (1 - a);
      const c = oklabFromLinear(r, g, b);
      lab[i * 3] = c[0]; lab[i * 3 + 1] = c[1]; lab[i * 3 + 2] = c[2];
    }
    return lab;
  }

  // Photographic look: open the shadows a little, roll off clipped highlights
  // and lift colour. Six inks are less saturated than any camera profile, so a
  // mild boost is what makes the panel read as a photo instead of a print test.
  function toneCurve(lab, style) {
    const photo = style === 'photo';
    const lift = photo ? .05 : .02;
    const knee = .88;
    const sat = photo ? 1.08 : 1.03;
    const s = photo ? .10 : .04;
    for (let i = 0; i < lab.length; i += 3) {
      let l = lab[i];
      l = l + lift * (1 - l) ** 3;
      if (l > knee) l = knee + (1 - knee) * (1 - Math.exp(-(l - knee) / (1 - knee)));
      l = l * (1 - s) + l * l * (3 - 2 * l) * s;
      lab[i] = clamp(l, 0, 1);
      lab[i + 1] *= sat;
      lab[i + 2] *= sat;
    }
  }

  // Chroma-only smoothing: photo noise mostly shows up as coloured speckle,
  // while the eye tolerates blurrier a/b than L. Keeps edges and luma detail.
  function smoothChroma(lab, width, height, passes) {
    if (passes <= 0) return lab;
    const n = width * height, k = [0.25, 0.5, 0.25];
    let a = new Float32Array(n), b = new Float32Array(n);
    for (let i = 0; i < n; i++) { a[i] = lab[i * 3 + 1]; b[i] = lab[i * 3 + 2]; }
    const blur = (src, axis) => {
      const out = new Float32Array(n);
      for (let y = 0; y < height; y++) {
        for (let x = 0; x < width; x++) {
          let sum = 0, weight = 0;
          for (let d = -1; d <= 1; d++) {
            const xx = axis === 0 ? x + d : x, yy = axis === 0 ? y : y + d;
            if (xx < 0 || xx >= width || yy < 0 || yy >= height) continue;
            sum += src[yy * width + xx] * k[d + 1]; weight += k[d + 1];
          }
          out[y * width + x] = sum / weight;
        }
      }
      return out;
    };
    for (let p = 0; p < passes; p++) { a = blur(blur(a, 0), 1); b = blur(blur(b, 0), 1); }
    for (let i = 0; i < n; i++) { lab[i * 3 + 1] = a[i]; lab[i * 3 + 2] = b[i]; }
    return lab;
  }

  function nearestInk(cL, cA, cB, inks) {
    let best = inks[0], distance = Infinity;
    for (let k = 0; k < inks.length; k++) {
      const ink = inks[k];
      const dl = cL - ink.lab[0], da = cA - ink.lab[1], db = cB - ink.lab[2];
      const d = dl * dl + da * da + db * db;
      if (d < distance) { distance = d; best = ink; }
    }
    return best;
  }

  // Serpentine Floyd-Steinberg in OKLab. Two error rows are enough; residuals
  // are clamped so a single out-of-gamut pixel cannot start a colour runaway.
  function* ditherSteps(lab, options) {
    const { width: W, height: H } = logicalSize();
    const { gray = false, quality = 'best', style = 'photo', canceled = () => false,
      neutralGate = 0 } = options;
    if (!Object.hasOwn(MODES, quality)) throw new Error('未知混色质量');
    if (!['photo', 'illustration', 'flat'].includes(style)) throw new Error('未知呈现风格');
    const inks = gray ? palette.slice(0, 2) : palette;
    const monoInks = palette.slice(0, 2);
    const gain = style === 'flat' ? 0 : MODES[quality];
    const ids = new Uint8Array(W * H);
    const preview = new Uint8ClampedArray(ids.length * 4);
    let cur = gain ? new Float32Array((W + 2) * 3) : null;
    let next = gain ? new Float32Array(cur.length) : null;
    const residualLimit = .5;
    const clock = typeof performance !== 'undefined' && performance.now ? () => performance.now() : () => Date.now();
    let sliceStarted = clock();
    for (let y = 0; y < H; y++) {
      if (canceled()) throw canceledError();
      const dir = y & 1 ? -1 : 1;
      for (let step = 0; step < W; step++) {
        const x = dir === 1 ? step : W - 1 - step;
        const i = y * W + x, p = i * 3, e = (x + 1) * 3;
        // Low-chroma source pixels are printed with black/white only. Mixing
        // yellow+blue to fake a grey produces the coloured speckle that makes
        // the old renderer look dirty, especially on skin and overcast skies.
        const neutral = gray || Math.hypot(lab[p + 1], lab[p + 2]) < neutralGate;
        let cL = lab[p], cA = lab[p + 1], cB = lab[p + 2];
        if (gain) { cL += cur[e]; cA += cur[e + 1]; cB += cur[e + 2]; }
        if (neutral) cA = cB = 0;
        const ink = nearestInk(cL, cA, cB, neutral ? monoInks : inks);
        ids[i] = ink.id;
        preview[i * 4] = ink.rgb[0]; preview[i * 4 + 1] = ink.rgb[1];
        preview[i * 4 + 2] = ink.rgb[2]; preview[i * 4 + 3] = 255;
        if (gain) {
          const eL = clamp(cL - ink.lab[0], -residualLimit, residualLimit) * gain;
          const eA = neutral ? 0 : clamp(cA - ink.lab[1], -residualLimit, residualLimit) * gain;
          const eB = neutral ? 0 : clamp(cB - ink.lab[2], -residualLimit, residualLimit) * gain;
          cur[e + dir * 3] += eL * 7 / 16; cur[e + dir * 3 + 1] += eA * 7 / 16; cur[e + dir * 3 + 2] += eB * 7 / 16;
          next[e - dir * 3] += eL * 3 / 16; next[e - dir * 3 + 1] += eA * 3 / 16; next[e - dir * 3 + 2] += eB * 3 / 16;
          next[e] += eL * 5 / 16; next[e + 1] += eA * 5 / 16; next[e + 2] += eB * 5 / 16;
          next[e + dir * 3] += eL / 16; next[e + dir * 3 + 1] += eA / 16; next[e + dir * 3 + 2] += eB / 16;
        }
      }
      if (gain) { const swap = cur; cur = next; next = swap; next.fill(0); }
      if (clock() - sliceStarted > 8) { sliceStarted = clock(); yield; }
    }
    return { ids, preview };
  }

  function* quantizeSteps(rgba, options = {}) {
    const { width: W, height: H } = logicalSize();
    const { gray = false, quality = 'best', style = 'photo', canceled = () => false } = options;
    if (!['photo', 'illustration', 'flat'].includes(style)) throw new Error('未知呈现风格');
    if (!rgba || rgba.length !== W * H * 4) throw new Error(`裁剪图尺寸必须为 ${W}×${H}`);
    const denoise = options.denoise ?? 'off';
    if (!['off', 'balanced', 'strong'].includes(denoise)) throw new Error('未知六色降噪等级');
    const lab = sourceLab(rgba, W, H);
    if (style !== 'flat') {
      toneCurve(lab, style);
      smoothChroma(lab, W, H, denoise === 'strong' ? 4 : denoise === 'balanced' ? 2 : 0);
    }
    const iterator = ditherSteps(lab, { gray, quality, style, canceled,
      neutralGate: options.neutralGate ?? (style === 'flat' ? .03 : 0) });
    for (;;) {
      const step = iterator.next();
      if (step.done) return step.value;
      yield;
    }
  }

  function quantize(rgba, options) {
    const it = quantizeSteps(rgba, options);
    let step;
    do { step = it.next(); } while (!step.done);
    return step.value;
  }
  async function quantizeAsync(rgba, options) {
    const it = quantizeSteps(rgba, options);
    for (;;) {
      const step = it.next();
      if (step.done) return step.value;
      await new Promise(resolve => setTimeout(resolve, 0));
    }
  }
  function checksum(bytes) {
    let hash = 0x811c9dc5;
    for (const byte of bytes) hash = Math.imul(hash ^ byte, 0x01000193) >>> 0;
    return hash.toString(16).padStart(8, '0');
  }
  function makeFrame(ids, quality = 'best') {
    if (!Object.hasOwn(MODES, quality)) throw new Error('未知混色质量');
    if (!ids || ids.length !== WIDTH * HEIGHT) throw new Error('像素数量错误');
    const body = new Uint8Array(FRAME_BYTES), counts = new Uint32Array(7);
    let offset = 0;
    for (let x = WIDTH - 1; x >= 0; x--) {
      for (let y = 0; y < HEIGHT; y += 2) {
        const a = ids[sourceIndex(x, y)], b = ids[sourceIndex(x, y + 1)];
        if (!validInk(a) || !validInk(b)) throw new Error('六色帧包含无效色号');
        body[offset++] = (a << 4) | b; counts[a]++; counts[b]++;
      }
    }
    return { rotationEpoch, body, counts, format: FORMAT, quality, pass: 0, checksum: checksum(body) };
  }
  function previewIds(ids) {
    if (!(ids instanceof Uint8Array) || ids.length !== WIDTH * HEIGHT) throw new Error('像素数量错误');
    const lookup = [];
    for (const color of palette) lookup[color.id] = color.rgb;
    const preview = new Uint8ClampedArray(ids.length * 4);
    for (let i = 0; i < ids.length; i++) {
      const rgb = lookup[ids[i]];
      if (!rgb) throw new Error('六色预览包含无效色号');
      preview.set(rgb, i * 4); preview[i * 4 + 3] = 255;
    }
    return preview;
  }
  function composeRegion(base, next, region) { const {width:WIDTH,height:HEIGHT}=logicalSize();
    if (!(base instanceof Uint8Array) || !(next instanceof Uint8Array) || base.constructor !== next.constructor ||
        base.length !== WIDTH * HEIGHT || next.length !== base.length) throw new Error('区域更新需要完整基准画面');
    const {x, y, width, height} = region || {};
    if (![x,y,width,height].every(Number.isInteger) || x < 0 || y < 0 || width < 1 || height < 1 ||
        x + width > WIDTH || y + height > HEIGHT) throw new Error('更新区域超出屏幕');
    const result = base.slice();
    for (let row = y; row < y + height; row++) {
      const start = row * WIDTH + x;
      result.set(next.subarray(start, start + width), start);
    }
    return result;
  }
  async function verifyBaseline(baseline) {
    if (!baseline || !Number.isInteger(baseline.id) || !Number.isInteger(baseline.generation) || baseline.generation < 1) {
      throw new Error('请先完整发送一次画面，建立区域更新基准');
    }
    const job = await request('/api/job', {cache:'no-store'}, 4000);
    if (!job.ok || job.phase !== 'idle' || job.err !== 0 || job.id !== baseline.id ||
        job.display_generation !== baseline.generation || job.current_display_generation !== baseline.generation) {
      throw new Error('屏幕基准已变化，请重新发送完整画面');
    }
    return job;
  }
  // Read bodies before clearing the timer, so a stalled response also times out.
  async function request(url, options = {}, timeoutMs = 15000) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    try {
      const res = await fetch(url, { ...options, signal: controller.signal });
      const body = await res.text();
      if (!res.ok) throw new Error(body && !body.trimStart().startsWith('<') ? body : `HTTP ${res.status}`);
      return JSON.parse(body);
    } catch (err) {
      if (err.name === 'AbortError') throw Object.assign(new Error('设备响应超时，请检查连接及设备日志'),
        { name: 'TimeoutError', retryable: true });
      throw err;
    } finally { clearTimeout(timer); }
  }
  async function sendFrame(frame, onPhase = () => {}, slot = null) {
    if (!frame || frame.format !== FORMAT || frame.pass !== 0 || !(frame.body instanceof Uint8Array) ||
        frame.body.length !== FRAME_BYTES || !Object.hasOwn(MODES, frame.quality) ||
        checksum(frame.body) !== frame.checksum || (frame.baseGeneration !== undefined &&
          (!Number.isInteger(frame.baseGeneration) || frame.baseGeneration < 1)) ||
        (slot !== null && (!Number.isInteger(slot) || slot < 0 || slot > 2))) {
      throw new Error('帧格式或校验值错误');
    }
    onPhase('正在发送六色画面，请等待屏幕完成刷新…');
    const accepted = await request('/api/frame', {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream', 'X-Eink-Format': frame.format,
        'X-Eink-Rotation-Epoch': String(frame.rotationEpoch), 'X-Eink-Pass': '0', 'X-Eink-Quality': frame.quality, 'X-Eink-Checksum': frame.checksum,
        ...(frame.baseGeneration ? {'X-Eink-Base-Generation': String(frame.baseGeneration)} : {}),
        ...(slot === null ? {} : {'X-Eink-Slot': String(slot)}) },
      body: frame.body,
    }, 240000);
    if (!accepted.ok || !Number.isInteger(accepted.id) || accepted.id <= 0) throw new Error('设备协议不匹配，请更新固件并刷新网页');
    if (accepted.phase === 'error' || accepted.err) throw new Error(`屏幕刷新失败 (${accepted.err})`);
    let completed = accepted;
    if (accepted.phase !== 'idle') {
      onPhase('图片已接收，屏幕刷新中…');
      completed = await waitForJob(accepted.id, 180000);
    }
    if (completed.kind !== 'frame' || completed.sequence !== FORMAT || completed.err !== 0 ||
        completed.completed_mask !== 1 || completed.plane_fnv?.length !== 1 ||
        completed.plane_fnv[0] !== frame.checksum ||
        !Number.isInteger(completed.display_generation) || completed.display_generation < 1)
      throw new Error('六色刷新完成凭据不匹配，请更新固件并刷新网页');
    return completed;
  }
  async function waitForJob(id, timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      const job = await request('/api/job', { cache: 'no-store' }, Math.min(15000, deadline - Date.now()));
      if (!job.ok || job.id !== id) throw new Error('刷新任务已变化，请重新发送');
      if (job.phase === 'error' || job.err) throw new Error(`屏幕刷新失败 (${job.err})`);
      if (job.phase === 'idle') return job;
      if (job.phase !== 'refresh' && job.phase !== 'receiving') throw new Error('设备返回未知刷新状态');
      await new Promise(resolve => setTimeout(resolve, 700));
    }
    throw new Error('屏幕刷新超时，请返回设备日志');
  }
  async function clearScreen() {
    const job = await request('/api/clear', {
      method: 'POST', body: '',
    });
    if (!job.ok || !Number.isInteger(job.id) || job.id <= 0 || job.kind !== 'clear-screen' ||
        job.sequence !== 'otp6-clear-v1')
      throw new Error('全局清屏请求协议不匹配');
    const completed = await waitForJob(job.id, 180000);
    if (completed.kind !== 'clear-screen' || completed.sequence !== 'otp6-clear-v1' ||
        completed.err !== 0 || completed.completed_mask !== 1 ||
        !Number.isInteger(completed.display_generation) || completed.display_generation < 1)
      throw new Error('全局清屏完成凭据不匹配');
    return completed;
  }
  return Object.freeze({ WIDTH, HEIGHT, FRAME_BYTES, FORMAT, COLORS,
    setPanelRotation, quantize, quantizeAsync, checksum, makeFrame,
    previewIds, composeRegion, verifyBaseline, sendFrame, clearScreen });
});
