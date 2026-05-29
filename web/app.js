const WIDTH = 240;
const HEIGHT = 160;
const REG = 0x04000000;
const PAL = 0x05000000;
const VRAM = 0x06000000;
const OAM = 0x07000000;
const KEYINPUT = 0x04000130;
const KEY_MASK = 0x03ff;
const searchParams = new URLSearchParams(location.search);
const speedParam = searchParams.get('speed');
const automate = searchParams.get('automate') === '1';
const MIN_SPEED = 0.1;
const MAX_SPEED = 10000;
const MIN_SPEED_EXPONENT = Math.log10(MIN_SPEED);
const MAX_SPEED_EXPONENT = Math.log10(MAX_SPEED);
const FAST_FRAME_BUDGET_MS = 16;

const buttons = {
  a: 1 << 0,
  b: 1 << 1,
  select: 1 << 2,
  start: 1 << 3,
  right: 1 << 4,
  left: 1 << 5,
  up: 1 << 6,
  down: 1 << 7,
  r: 1 << 8,
  l: 1 << 9,
};

const keyMap = new Map([
  ['KeyZ', 'a'], ['KeyX', 'b'], ['ShiftLeft', 'select'], ['ShiftRight', 'select'], ['Enter', 'start'],
  ['ArrowRight', 'right'], ['ArrowLeft', 'left'], ['ArrowUp', 'up'], ['ArrowDown', 'down'],
  ['KeyS', 'r'], ['KeyA', 'l'],
]);

const canvas = document.querySelector('#screen');
const statusEl = document.querySelector('#status');
const speedInput = document.querySelector('#speed');
const speedValue = document.querySelector('#speed-value');
const ctx = canvas.getContext('2d');
const image = ctx.createImageData(WIDTH, HEIGHT);
const layerData = new Uint8Array(WIDTH * HEIGHT);
const pressed = new Set();
const pendingPresses = new Map();

let instance;
let memory;
let u8;
let u16;
let statusText = 'loading wasm…';
let lastFpsUpdate = performance.now();
let lastTick = performance.now();
let renderedFrames = 0;
let emulatedFrames = 0;
let gameFrameAccumulator = 0;
let speed = 1;
let currentFrame = 0;
let automationReady;
let resolveAutomationReady;
if (automate) {
  automationReady = new Promise((resolve) => { resolveAutomationReady = resolve; });
}

function refreshViews() {
  u8 = new Uint8Array(memory.buffer);
  u16 = new Uint16Array(memory.buffer);
}

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function speedToExponent(value) {
  return Math.log10(clamp(value, MIN_SPEED, MAX_SPEED));
}

function exponentToSpeed(value) {
  return 10 ** clamp(value, MIN_SPEED_EXPONENT, MAX_SPEED_EXPONENT);
}

function formatSpeed(value) {
  if (value < 1) return `${value.toFixed(1)}x`;
  if (value < 100) return `${value.toFixed(value < 10 ? 1 : 0)}x`;
  return `${Math.round(value)}x`;
}

function setSpeedFromExponent(exponent) {
  speed = exponentToSpeed(Number(exponent));
  speedInput.value = String(speedToExponent(speed));
  speedValue.textContent = formatSpeed(speed);
}

function resetFpsCounters() {
  lastFpsUpdate = performance.now();
  renderedFrames = 0;
  emulatedFrames = 0;
  gameFrameAccumulator = 0;
}

function initialSpeed() {
  if (speedParam === '0') return MAX_SPEED;
  const parsed = Number(speedParam);
  return Number.isFinite(parsed) && parsed > 0 ? parsed : 1;
}

function gbaColor(value) {
  const r = (value & 31) * 255 / 31;
  const g = ((value >> 5) & 31) * 255 / 31;
  const b = ((value >> 10) & 31) * 255 / 31;
  return [r | 0, g | 0, b | 0];
}

function inWindowRange(value, range) {
  const start = range >> 8;
  const end = range & 0xff;
  return start <= end ? value >= start && value < end : value >= start || value < end;
}

function windowMask(x, y) {
  const dispcnt = u16[REG >> 1];
  const windowsEnabled = dispcnt & 0xe000;
  if (!windowsEnabled) return 0x3f;

  if ((dispcnt & 0x2000)
      && inWindowRange(x, u16[(REG + 0x40) >> 1])
      && inWindowRange(y, u16[(REG + 0x44) >> 1])) {
    return u16[(REG + 0x48) >> 1] & 0x3f;
  }

  if ((dispcnt & 0x4000)
      && inWindowRange(x, u16[(REG + 0x42) >> 1])
      && inWindowRange(y, u16[(REG + 0x46) >> 1])) {
    return (u16[(REG + 0x48) >> 1] >> 8) & 0x3f;
  }

  return u16[(REG + 0x4a) >> 1] & 0x3f;
}

function activeBlendColor(color, layer, pixel, effectsEnabled) {
  const bldcnt = u16[(REG + 0x50) >> 1];
  const effect = (bldcnt >> 6) & 3;
  const sourceTargets = bldcnt & 0x3f;
  if (!effectsEnabled || !(sourceTargets & layer) || effect === 0) return color;

  if (effect === 1 && (bldcnt >> 8) & layerData[pixel]) {
    const alpha = u16[(REG + 0x52) >> 1];
    const eva = Math.min(alpha & 0x1f, 16);
    const evb = Math.min((alpha >> 8) & 0x1f, 16);
    return [
      Math.min(255, (color[0] * eva + image.data[pixel * 4] * evb) >> 4),
      Math.min(255, (color[1] * eva + image.data[pixel * 4 + 1] * evb) >> 4),
      Math.min(255, (color[2] * eva + image.data[pixel * 4 + 2] * evb) >> 4),
    ];
  }

  const evy = Math.min(u16[(REG + 0x54) >> 1] & 0x1f, 16);
  if (effect === 2) {
    return color.map((component) => component + (((255 - component) * evy) >> 4));
  }
  if (effect === 3) {
    return color.map((component) => component - ((component * evy) >> 4));
  }
  return color;
}

function putPixel(x, y, color, layer = 0x20) {
  if (x < 0 || y < 0 || x >= WIDTH || y >= HEIGHT) return;
  const mask = windowMask(x, y);
  if (!(mask & layer)) return;

  const pixel = y * WIDTH + x;
  const output = activeBlendColor(color, layer, pixel, mask & 0x20);
  const p = pixel * 4;
  image.data[p] = output[0];
  image.data[p + 1] = output[1];
  image.data[p + 2] = output[2];
  image.data[p + 3] = 255;
  layerData[pixel] = layer;
}

function clearScreen() {
  const color = gbaColor(u16[PAL >> 1]);
  for (let y = 0; y < HEIGHT; y++) {
    for (let x = 0; x < WIDTH; x++) putPixel(x, y, color, 0x20);
  }
}

function renderBitmapMode3() {
  for (let i = 0; i < WIDTH * HEIGHT; i++) {
    const [r, g, b] = gbaColor(u16[(VRAM >> 1) + i]);
    const p = i * 4;
    image.data[p] = r;
    image.data[p + 1] = g;
    image.data[p + 2] = b;
    image.data[p + 3] = 255;
    layerData[i] = 0x04;
  }
}

function renderBitmapMode4(dispcnt) {
  const page = dispcnt & 0x10 ? 0xA000 : 0;
  for (let i = 0; i < WIDTH * HEIGHT; i++) {
    const colorIndex = u8[VRAM + page + i];
    const [r, g, b] = gbaColor(u16[(PAL >> 1) + colorIndex]);
    const p = i * 4;
    image.data[p] = r;
    image.data[p + 1] = g;
    image.data[p + 2] = b;
    image.data[p + 3] = 255;
    layerData[i] = 0x04;
  }
}

function signed16(value) {
  return value << 16 >> 16;
}

function signed28(value) {
  return value << 4 >> 4;
}

function word(offset) {
  const lo = u16[(REG + offset) >> 1];
  const hi = u16[(REG + offset + 2) >> 1];
  return lo | (hi << 16);
}

function textBgPixel(bg, x, y) {
  const cnt = u16[(REG + 8 + bg * 2) >> 1];
  const charBase = VRAM + ((cnt >> 2) & 3) * 0x4000;
  const screenBase = VRAM + ((cnt >> 8) & 31) * 0x800;
  const color256 = cnt & 0x80;
  const size = (cnt >> 14) & 3;
  const width = size & 1 ? 512 : 256;
  const height = size & 2 ? 512 : 256;
  const hofs = u16[(REG + 0x10 + bg * 4) >> 1] & 511;
  const vofs = u16[(REG + 0x12 + bg * 4) >> 1] & 511;
  const sx = (x + hofs) & (width - 1);
  const sy = (y + vofs) & (height - 1);
  const block = (sx >= 256 ? 1 : 0) + (sy >= 256 ? (size === 3 ? 2 : 1) : 0);
  const mapX = (sx & 255) >> 3;
  const mapY = (sy & 255) >> 3;
  const entry = u16[(screenBase + block * 0x800 + (mapY * 32 + mapX) * 2) >> 1];
  const tile = entry & 0x3ff;
  const palette = (entry >> 12) & 15;
  const px = entry & 0x400 ? 7 - (sx & 7) : sx & 7;
  const py = entry & 0x800 ? 7 - (sy & 7) : sy & 7;

  let colorIndex;
  if (color256) {
    colorIndex = u8[charBase + tile * 64 + py * 8 + px];
    if (!colorIndex) return null;
    return gbaColor(u16[(PAL >> 1) + colorIndex]);
  }

  const packed = u8[charBase + tile * 32 + py * 4 + (px >> 1)];
  colorIndex = px & 1 ? packed >> 4 : packed & 15;
  if (!colorIndex) return null;
  return gbaColor(u16[(PAL >> 1) + palette * 16 + colorIndex]);
}

function affineBgPixel(bg, x, y) {
  const cnt = u16[(REG + 8 + bg * 2) >> 1];
  const charBase = VRAM + ((cnt >> 2) & 3) * 0x4000;
  const screenBase = VRAM + ((cnt >> 8) & 31) * 0x800;
  const sizes = [128, 256, 512, 1024];
  const size = sizes[(cnt >> 14) & 3];
  const wrap = cnt & 0x2000;
  const reg = bg === 2 ? 0x20 : 0x30;
  const pa = signed16(u16[(REG + reg) >> 1]);
  const pb = signed16(u16[(REG + reg + 2) >> 1]);
  const pc = signed16(u16[(REG + reg + 4) >> 1]);
  const pd = signed16(u16[(REG + reg + 6) >> 1]);
  const refX = signed28(word(reg + 8));
  const refY = signed28(word(reg + 12));
  let sx = (refX + pa * x + pb * y) >> 8;
  let sy = (refY + pc * x + pd * y) >> 8;

  if (wrap) {
    sx &= size - 1;
    sy &= size - 1;
  } else if (sx < 0 || sy < 0 || sx >= size || sy >= size) {
    return null;
  }

  const tilesPerRow = size >> 3;
  const tile = u8[screenBase + (sy >> 3) * tilesPerRow + (sx >> 3)];
  const colorIndex = u8[charBase + tile * 64 + (sy & 7) * 8 + (sx & 7)];
  if (!colorIndex) return null;
  return gbaColor(u16[(PAL >> 1) + colorIndex]);
}

function bgLayersForMode(dispcnt) {
  const mode = dispcnt & 7;
  const layers = [];
  for (let bg = 0; bg < 4; bg++) {
    if (!(dispcnt & (0x100 << bg))) continue;
    if (mode === 0) layers.push({ bg, type: 'text' });
    else if (mode === 1 && bg < 2) layers.push({ bg, type: 'text' });
    else if (mode === 1 && bg === 2) layers.push({ bg, type: 'affine' });
    else if (mode === 2 && bg >= 2) layers.push({ bg, type: 'affine' });
  }
  return layers.map((layer) => ({
    ...layer,
    priority: u16[(REG + 8 + layer.bg * 2) >> 1] & 3,
  }));
}

function objTileOffset(tileBase, tileX, tileY, width, color256, mapping1d) {
  return mapping1d
    ? tileBase + tileY * (color256 ? width >> 2 : width >> 3) + tileX * (color256 ? 2 : 1)
    : tileBase + tileY * 32 + tileX * (color256 ? 2 : 1);
}

function objPixel(tileBase, x, y, width, color256, palette, mapping1d) {
  const tileOffset = objTileOffset(tileBase, x >> 3, y >> 3, width, color256, mapping1d);
  let colorIndex;
  if (color256) colorIndex = u8[VRAM + 0x10000 + tileOffset * 32 + (y & 7) * 8 + (x & 7)];
  else {
    const packed = u8[VRAM + 0x10000 + tileOffset * 32 + (y & 7) * 4 + ((x & 7) >> 1)];
    colorIndex = x & 1 ? packed >> 4 : packed & 15;
  }
  if (!colorIndex) return null;
  const palOffset = color256 ? colorIndex : palette * 16 + colorIndex;
  return gbaColor(u16[(PAL >> 1) + 0x100 + palOffset]);
}

function renderBgLayer(bg, type) {
  const pixel = type === 'affine' ? affineBgPixel : textBgPixel;
  const layer = 1 << bg;
  for (let y = 0; y < HEIGHT; y++) {
    for (let x = 0; x < WIDTH; x++) {
      const color = pixel(bg, x, y);
      if (color) putPixel(x, y, color, layer);
    }
  }
}

function renderBgs(dispcnt) {
  clearScreen();
  for (const { bg, type } of bgLayersForMode(dispcnt).sort((a, b) => b.priority - a.priority)) {
    renderBgLayer(bg, type);
  }
}

function renderSprites(dispcnt, priority = null) {
  if (!(dispcnt & 0x1000)) return;
  const mapping1d = dispcnt & 0x40;
  const sizes = [
    [[8, 8], [16, 16], [32, 32], [64, 64]],
    [[16, 8], [32, 8], [32, 16], [64, 32]],
    [[8, 16], [8, 32], [16, 32], [32, 64]],
  ];

  for (let i = 127; i >= 0; i--) {
    const base = (OAM >> 1) + i * 4;
    const a0 = u16[base];
    const a1 = u16[base + 1];
    const a2 = u16[base + 2];
    const affineMode = (a0 >> 8) & 3;
    const affine = affineMode & 1;
    if (!affine && (a0 & 0x0200)) continue;
    const shape = (a0 >> 14) & 3;
    if (shape === 3) continue;
    const [w, h] = sizes[shape][(a1 >> 14) & 3];
    const color256 = a0 & 0x2000;
    const spritePriority = (a2 >> 10) & 3;
    if (priority !== null && spritePriority !== priority) continue;
    const palette = (a2 >> 12) & 15;
    const tileBase = a2 & 0x3ff;
    let ox = a1 & 511;
    let oy = a0 & 255;
    if (ox > 240) ox -= 512;
    if (oy > 160) oy -= 256;

    if (affine) {
      const matrix = (a1 >> 9) & 31;
      const matrixBase = (OAM >> 1) + matrix * 16;
      const pa = signed16(u16[matrixBase + 3]);
      const pb = signed16(u16[matrixBase + 7]);
      const pc = signed16(u16[matrixBase + 11]);
      const pd = signed16(u16[matrixBase + 15]);
      const drawW = affineMode === 3 ? w * 2 : w;
      const drawH = affineMode === 3 ? h * 2 : h;
      const drawCx = drawW / 2;
      const drawCy = drawH / 2;
      const texCx = w / 2;
      const texCy = h / 2;

      for (let y = 0; y < drawH; y++) {
        for (let x = 0; x < drawW; x++) {
          const dx = x - drawCx;
          const dy = y - drawCy;
          const px = ((pa * dx + pb * dy) >> 8) + texCx;
          const py = ((pc * dx + pd * dy) >> 8) + texCy;
          if (px < 0 || py < 0 || px >= w || py >= h) continue;
          const color = objPixel(tileBase, px, py, w, color256, palette, mapping1d);
          if (color) putPixel(ox + x, oy + y, color, 0x10);
        }
      }
    } else {
      for (let y = 0; y < h; y++) {
        for (let x = 0; x < w; x++) {
          const px = a1 & 0x1000 ? w - 1 - x : x;
          const py = a1 & 0x2000 ? h - 1 - y : y;
          const color = objPixel(tileBase, px, py, w, color256, palette, mapping1d);
          if (color) putPixel(ox + x, oy + y, color, 0x10);
        }
      }
    }
  }
}

function renderTiled(dispcnt) {
  clearScreen();
  const layers = bgLayersForMode(dispcnt);
  for (let priority = 3; priority >= 0; priority--) {
    for (const { bg, type } of layers) {
      if ((u16[(REG + 8 + bg * 2) >> 1] & 3) === priority) renderBgLayer(bg, type);
    }
    renderSprites(dispcnt, priority);
  }
}

function render() {
  const dispcnt = u16[REG >> 1];
  const mode = dispcnt & 7;
  if (mode === 3) renderBitmapMode3();
  else if (mode === 4) renderBitmapMode4(dispcnt);
  else renderTiled(dispcnt);
  if (mode === 3 || mode === 4) renderSprites(dispcnt);
  ctx.putImageData(image, 0, 0);
}

function copy(src, dst, count, size, fill) {
  for (let i = 0; i < count; i++) {
    const from = fill ? src : src + i * size;
    u8.set(u8.subarray(from, from + size), dst + i * size);
  }
}

function lz77(src, dst) {
  const size = u8[src + 1] | (u8[src + 2] << 8) | (u8[src + 3] << 16);
  let s = src + 4;
  let d = dst;
  const end = dst + size;
  while (d < end) {
    const flags = u8[s++];
    for (let bit = 7; bit >= 0 && d < end; bit--) {
      if (flags & (1 << bit)) {
        const pair = (u8[s] << 8) | u8[s + 1];
        s += 2;
        let length = (pair >> 12) + 3;
        const disp = (pair & 0xfff) + 1;
        while (length-- && d < end) {
          u8[d] = u8[d - disp];
          d++;
        }
      } else {
        u8[d++] = u8[s++];
      }
    }
  }
}

function rl(src, dst) {
  const size = u8[src + 1] | (u8[src + 2] << 8) | (u8[src + 3] << 16);
  let s = src + 4;
  let d = dst;
  const end = dst + size;
  while (d < end) {
    const flag = u8[s++];
    if (flag & 0x80) {
      let count = (flag & 0x7f) + 3;
      const value = u8[s++];
      while (count-- && d < end) u8[d++] = value;
    } else {
      let count = (flag & 0x7f) + 1;
      while (count-- && d < end) u8[d++] = u8[s++];
    }
  }
}

function readCString(ptr) {
  let out = '';
  while (u8[ptr]) out += String.fromCharCode(u8[ptr++]);
  return out;
}

function readS16(ptr) {
  return u16[ptr >> 1] << 16 >> 16;
}

function readS32(ptr) {
  return (u16[ptr >> 1] | (u16[(ptr + 2) >> 1] << 16)) | 0;
}

function writeS16(ptr, value) {
  u16[ptr >> 1] = value & 0xffff;
}

function writeS32(ptr, value) {
  u16[ptr >> 1] = value & 0xffff;
  u16[(ptr + 2) >> 1] = (value >> 16) & 0xffff;
}

function affineTerms(xScale, yScale, rotation) {
  const angle = rotation * Math.PI * 2 / 256;
  const sin = Math.sin(angle) * 256;
  const cos = Math.cos(angle) * 256;
  return {
    pa: cos * xScale / 256,
    pb: -sin * yScale / 256,
    pc: sin * xScale / 256,
    pd: cos * yScale / 256,
  };
}

function bgAffineSet(src, dest, count) {
  for (let i = 0; i < count; i++) {
    const s = src + i * 20;
    const d = dest + i * 16;
    const texX = readS32(s);
    const texY = readS32(s + 4);
    const scrX = readS16(s + 8);
    const scrY = readS16(s + 10);
    const { pa, pb, pc, pd } = affineTerms(readS16(s + 12), readS16(s + 14), u16[(s + 16) >> 1]);
    const a = pa | 0;
    const b = pb | 0;
    const c = pc | 0;
    const e = pd | 0;
    writeS16(d, a);
    writeS16(d + 2, b);
    writeS16(d + 4, c);
    writeS16(d + 6, e);
    writeS32(d + 8, (texX - scrX * a - scrY * b) | 0);
    writeS32(d + 12, (texY - scrX * c - scrY * e) | 0);
  }
}

function objAffineSet(src, dest, count, offset) {
  for (let i = 0; i < count; i++) {
    const s = src + i * 6;
    const d = dest + i * offset * 4;
    const { pa, pb, pc, pd } = affineTerms(readS16(s), readS16(s + 2), u16[(s + 4) >> 1]);
    writeS16(d, pa | 0);
    writeS16(d + offset, pb | 0);
    writeS16(d + offset * 2, pc | 0);
    writeS16(d + offset * 3, pd | 0);
  }
}

function importsFor(module) {
  const env = {};
  for (const item of WebAssembly.Module.imports(module)) {
    if (item.kind !== 'function') continue;
    env[item.name] = (...args) => {
      switch (item.name) {
        case 'CpuSet': return copy(args[0], args[1], args[2] & 0x1fffff, (args[2] >>> 26) & 1 ? 4 : 2, (args[2] >>> 24) & 1);
        case 'CpuFastSet': return copy(args[0], args[1], args[2] & 0x1fffff, 4, (args[2] >>> 24) & 1);
        case 'LZ77UnCompWram':
        case 'LZ77UnCompVram': return lz77(args[0], args[1]);
        case 'RLUnCompWram':
        case 'RLUnCompVram': return rl(args[0], args[1]);
        case 'BgAffineSet': return bgAffineSet(args[0], args[1], args[2]);
        case 'ObjAffineSet': return objAffineSet(args[0], args[1], args[2], args[3]);
        case 'Div': return args[1] ? (args[0] / args[1]) | 0 : 0;
        case 'Sqrt': return Math.sqrt(args[0]) | 0;
        case 'strcmp': return readCString(args[0]).localeCompare(readCString(args[1]));
        default: return 0;
      }
    };
  }
  return { env };
}

function writeKeys() {
  let held = 0;
  for (const key of pressed) held |= buttons[key] || 0;
  for (const key of pendingPresses.keys()) held |= buttons[key] || 0;
  u16[KEYINPUT >> 1] = KEY_MASK ^ held;
}

function stepPendingPresses() {
  for (const [key, frames] of pendingPresses) {
    if (frames <= 1) pendingPresses.delete(key);
    else pendingPresses.set(key, frames - 1);
  }
}

function setPressed(name, isPressed) {
  if (isPressed) {
    pressed.add(name);
    pendingPresses.set(name, 12);
  } else {
    pressed.delete(name);
  }
  document.querySelectorAll(`[data-key='${name}']`).forEach((el) => el.classList.toggle('pressed', isPressed));
  if (u16) writeKeys();
}

window.addEventListener('keydown', (event) => {
  const name = keyMap.get(event.code);
  if (!name) return;
  event.preventDefault();
  setPressed(name, true);
});

window.addEventListener('keyup', (event) => {
  const name = keyMap.get(event.code);
  if (!name) return;
  event.preventDefault();
  setPressed(name, false);
});

document.querySelectorAll('[data-key]').forEach((button) => {
  const name = button.dataset.key;
  button.addEventListener('pointerdown', (event) => { event.preventDefault(); setPressed(name, true); });
  button.addEventListener('pointerup', () => setPressed(name, false));
  button.addEventListener('pointercancel', () => setPressed(name, false));
  button.addEventListener('pointerleave', () => setPressed(name, false));
});

speedInput.addEventListener('input', () => {
  setSpeedFromExponent(speedInput.value);
  resetFpsCounters();
});

async function boot() {
  const bytes = await fetch('/build/wasm/pokeemerald.wasm', { cache: 'no-store' }).then((res) => res.arrayBuffer());
  const module = await WebAssembly.compile(bytes);
  instance = await WebAssembly.instantiate(module, importsFor(module));
  memory = instance.exports.memory;
  window.pokeemerald = { instance, memory, runFrames };
  if (automate) window.pokeemerald.automation = automationApi();
  refreshViews();
  writeKeys();
  instance.exports.AgbMain();
  statusText = `running — ${(bytes.byteLength / 1024 / 1024).toFixed(1)} MiB wasm`;
  setSpeedFromExponent(speedToExponent(initialSpeed()));
  statusEl.textContent = `${statusText} — Display FPS: 0, Game FPS: 0`;
  if (automate) {
    render();
    resolveAutomationReady();
  } else {
    requestAnimationFrame(tick);
  }
}

function updateFps(frameCount) {
  renderedFrames++;
  emulatedFrames += frameCount;

  const now = performance.now();
  const elapsed = now - lastFpsUpdate;
  if (elapsed < 1000) return;

  const fps = Math.round(renderedFrames * 1000 / elapsed);
  const gameFps = Math.round(emulatedFrames * 1000 / elapsed);
  statusEl.textContent = `${statusText} — Display FPS: ${fps}, Game FPS: ${gameFps}`;
  lastFpsUpdate = now;
  renderedFrames = 0;
  emulatedFrames = 0;
}

function runFrames(frameCount, keyMask = 0) {
  for (let i = 0; i < frameCount; i++) {
    if (keyMask) u16[KEYINPUT >> 1] = KEY_MASK ^ keyMask;
    else writeKeys();
    instance.exports.WasmRunFrame();
    currentFrame++;
    stepPendingPresses();
  }
  u16[KEYINPUT >> 1] = KEY_MASK;
}

function setAutomationButton(name, isPressed) {
  if (!Object.hasOwn(buttons, name)) throw new Error(`unknown button: ${name}`);
  pendingPresses.delete(name);
  if (isPressed) pressed.add(name);
  else pressed.delete(name);
  document.querySelectorAll(`[data-key='${name}']`).forEach((el) => el.classList.toggle('pressed', isPressed));
  writeKeys();
}

function runToFrame(targetFrame) {
  if (!Number.isInteger(targetFrame) || targetFrame < currentFrame) {
    throw new Error(`cannot run from frame ${currentFrame} to ${targetFrame}`);
  }
  runFrames(targetFrame - currentFrame);
  render();
  return currentFrame;
}

function readU32(ptr) {
  return u16[ptr >> 1] | (u16[(ptr + 2) >> 1] << 16);
}

function automationState() {
  const saveBlock1 = readU32(instance.exports.gSaveBlock1Ptr.value);
  const playerAvatar = instance.exports.gPlayerAvatar.value;
  const objectEventId = u8[playerAvatar + 5];
  const objectEvent = instance.exports.gObjectEvents.value + objectEventId * 0x24;
  return {
    frame: currentFrame,
    x: readS16(saveBlock1),
    y: readS16(saveBlock1 + 2),
    mapGroup: u8[saveBlock1 + 4],
    mapNum: u8[saveBlock1 + 5],
    elevation: u8[objectEvent + 0x0b] & 0x0f,
    objectX: readS16(objectEvent + 0x10),
    objectY: readS16(objectEvent + 0x12),
    objectEventId,
    littlerootTownState: instance.exports.VarGet(0x4050),
    littlerootRivalState: instance.exports.VarGet(0x408d),
    littlerootIntroState: instance.exports.VarGet(0x4092),
  };
}

function automationApi() {
  return {
    ready: automationReady,
    setButton: setAutomationButton,
    runToFrame,
    screenshot: () => canvas.toDataURL('image/png'),
    state: automationState,
    frame: () => currentFrame,
  };
}

function runFramesForTick(elapsedMs) {
  gameFrameAccumulator += speed * elapsedMs / (1000 / 60);
  let frameCount = Math.floor(gameFrameAccumulator);
  gameFrameAccumulator -= frameCount;
  if (frameCount === 0) return 0;

  const start = performance.now();
  let frames = 0;
  while (frames < frameCount && performance.now() - start < FAST_FRAME_BUDGET_MS) {
    runFrames(1);
    frames++;
  }
  return frames;
}

function tick(now) {
  try {
    const elapsedMs = Math.min(now - lastTick, 100);
    lastTick = now;
    const frames = runFramesForTick(elapsedMs);
    render();
    updateFps(frames);
    requestAnimationFrame(tick);
  } catch (error) {
    console.error(error);
    statusEl.textContent = error.stack || String(error);
  }
}

boot().catch((error) => {
  console.error(error);
  statusEl.textContent = error.stack || String(error);
});
