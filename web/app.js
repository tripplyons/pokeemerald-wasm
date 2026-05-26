const WIDTH = 240;
const HEIGHT = 160;
const REG = 0x04000000;
const PAL = 0x05000000;
const VRAM = 0x06000000;
const OAM = 0x07000000;
const KEYINPUT = 0x04000130;
const KEY_MASK = 0x03ff;

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
  ['KeyZ', 'a'], ['KeyX', 'b'], ['Backspace', 'select'], ['Enter', 'start'],
  ['ArrowRight', 'right'], ['ArrowLeft', 'left'], ['ArrowUp', 'up'], ['ArrowDown', 'down'],
  ['KeyS', 'r'], ['KeyA', 'l'],
]);

const canvas = document.querySelector('#screen');
const statusEl = document.querySelector('#status');
const ctx = canvas.getContext('2d');
const image = ctx.createImageData(WIDTH, HEIGHT);
const pressed = new Set();

let instance;
let memory;
let u8;
let u16;

function refreshViews() {
  u8 = new Uint8Array(memory.buffer);
  u16 = new Uint16Array(memory.buffer);
}

function gbaColor(value) {
  const r = (value & 31) * 255 / 31;
  const g = ((value >> 5) & 31) * 255 / 31;
  const b = ((value >> 10) & 31) * 255 / 31;
  return [r | 0, g | 0, b | 0];
}

function putPixel(x, y, color) {
  if (x < 0 || y < 0 || x >= WIDTH || y >= HEIGHT) return;
  const p = (y * WIDTH + x) * 4;
  image.data[p] = color[0];
  image.data[p + 1] = color[1];
  image.data[p + 2] = color[2];
  image.data[p + 3] = 255;
}

function clearScreen() {
  const color = gbaColor(u16[PAL >> 1]);
  for (let y = 0; y < HEIGHT; y++) {
    for (let x = 0; x < WIDTH; x++) putPixel(x, y, color);
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

function renderBgLayer(bg, type) {
  const pixel = type === 'affine' ? affineBgPixel : textBgPixel;
  for (let y = 0; y < HEIGHT; y++) {
    for (let x = 0; x < WIDTH; x++) {
      const color = pixel(bg, x, y);
      if (color) putPixel(x, y, color);
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
    if (a0 & 0x0200) continue;
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
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const px = a1 & 0x1000 ? w - 1 - x : x;
        const py = a1 & 0x2000 ? h - 1 - y : y;
        const tileX = px >> 3;
        const tileY = py >> 3;
        const tileOffset = mapping1d
          ? tileBase + tileY * (color256 ? w >> 2 : w >> 3) + tileX * (color256 ? 2 : 1)
          : tileBase + tileY * 32 + tileX * (color256 ? 2 : 1);
        let colorIndex;
        if (color256) colorIndex = u8[VRAM + 0x10000 + tileOffset * 32 + (py & 7) * 8 + (px & 7)];
        else {
          const packed = u8[VRAM + 0x10000 + tileOffset * 32 + (py & 7) * 4 + ((px & 7) >> 1)];
          colorIndex = px & 1 ? packed >> 4 : packed & 15;
        }
        if (!colorIndex) continue;
        const palOffset = color256 ? colorIndex : palette * 16 + colorIndex;
        putPixel(ox + x, oy + y, gbaColor(u16[(PAL >> 1) + 0x100 + palOffset]));
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
        case 'CpuFastSet': return copy(args[0], args[1], (args[2] & 0x1fffff) * 8, 4, (args[2] >>> 24) & 1);
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
  u16[KEYINPUT >> 1] = KEY_MASK ^ held;
}

function setPressed(name, isPressed) {
  if (isPressed) pressed.add(name);
  else pressed.delete(name);
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

async function boot() {
  const bytes = await fetch('/build/wasm/pokeemerald.wasm', { cache: 'no-store' }).then((res) => res.arrayBuffer());
  const module = await WebAssembly.compile(bytes);
  instance = await WebAssembly.instantiate(module, importsFor(module));
  memory = instance.exports.memory;
  window.pokeemerald = { instance, memory };
  refreshViews();
  writeKeys();
  instance.exports.AgbMain();
  statusEl.textContent = `running — ${(bytes.byteLength / 1024 / 1024).toFixed(1)} MiB wasm`;
  setInterval(tick, 1000 / 60);
}

function tick() {
  try {
    writeKeys();
    instance.exports.WasmRunFrame();
    render();
  } catch (error) {
    console.error(error);
    statusEl.textContent = error.stack || String(error);
  }
}

boot().catch((error) => {
  console.error(error);
  statusEl.textContent = error.stack || String(error);
});
