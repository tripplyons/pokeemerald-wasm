const WIDTH = 240;
const HEIGHT = 160;
const REG = 0x04000000;
const KEYINPUT = 0x04000130;
const KEY_MASK = 0x03ff;
const FLASH_BASE = 0x0e000000;
const FLASH_SIZE = 128 * 1024;
const FLASH_SECTOR_SIZE = 4096;
const REG_OFFSET_WIN0H = 0x40;
const REG_OFFSET_WIN0V = 0x44;
const REG_OFFSET_WININ = 0x48;
const REG_OFFSET_WINOUT = 0x4a;
const REG_OFFSET_DMA0 = 0xb0;
const DMA_DEST_FIXED = 0x0040;
const DMA_DEST_RELOAD = 0x0060;
const DMA_REPEAT = 0x0200;
const DMA_START_HBLANK = 0x2000;
const DMA_ENABLE = 0x8000;
const SAVE_SECTORS_PER_SLOT = 14;
const SAVE_SECTOR_SIGNATURE = 0x08012025;
const SAVE_SECTOR_DATA_SIZES = [
  0x0f2c,
  0x0f80, 0x0f80, 0x0f80, 0x0f0c,
  0x0f80, 0x0f80, 0x0f80, 0x0f80, 0x0f80, 0x0f80, 0x0f80, 0x0f80, 0x07d0,
];
const VANILLA_SAVE_SECTOR_DATA_SIZES = [
  0x0f2c,
  0x0f80, 0x0f80, 0x0f80, 0x0f08,
  0x0f80, 0x0f80, 0x0f80, 0x0f80, 0x0f80, 0x0f80, 0x0f80, 0x0f80, 0x07d0,
];
const LEGACY_WASM_SAVE_SECTOR_DATA_SIZES = [
  0x0f08,
  0x0f80, 0x0f80, 0x0f80, 0x0dc4,
  0x0f80, 0x0f80, 0x0f80, 0x0f80, 0x0f80, 0x0f80, 0x0f80, 0x0f80, 0x07d0,
];
const LEGACY_WASM_FRONTEND_SAVE_SECTOR_DATA_SIZES = [
  0x0f08,
  0x0f80, 0x0f80, 0x0f80, 0x0dc0,
  0x0f80, 0x0f80, 0x0f80, 0x0f80, 0x0f80, 0x0f80, 0x0f80, 0x0f80, 0x07d0,
];
const SAVE_BLOCK2_SIZE = 0x0f2c;
const SAVE_BLOCK1_SIZE = 0x3d8c;
const LEGACY_SAVE_BLOCK2_SIZE = 0x0f08;
const LEGACY_SAVE_BLOCK1_SIZE = 0x3c44;
const SAVE_BLOCK2_ENCRYPTION_KEY_OFFSET = 0x0ac;
const SAVE_BLOCK1_COINS_OFFSET = 0x494;
const SAVE_BAG_POCKETS = [
  [0x560, 30, 99],
  [0x5d8, 30, 99],
  [0x650, 16, 99],
  [0x690, 64, 99],
  [0x790, 46, 999],
];
const SAVE_STORAGE_KEY = 'pokeemerald.wasm.flash.v1';
const SAVE_FLUSH_INTERVAL_MS = 1000;
const searchParams = new URLSearchParams(location.search);
const speedParam = searchParams.get('speed');
const automate = searchParams.get('automate') === '1';
const MIN_SPEED = 0.1;
const MAX_SPEED = 1000;
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
const speedButtons = document.querySelectorAll('[data-speed]');
const speedValue = document.querySelector('#speed-value');
const fullscreenButton = document.querySelector('#fullscreen');
const shell = document.querySelector('.shell');
const downloadSaveButton = document.querySelector('#download-save');
const uploadSaveInput = document.querySelector('#upload-save');
const ctx = canvas.getContext('2d');
let image;
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
let lastFiniteSpeed = 1;
let currentFrame = 0;
let lastSavedFlashHash = 0;
let lastSaveFlushTime = performance.now();
let wasmModulePromise;
let bootId = 0;
let automationReady;
let resolveAutomationReady;
if (automate) {
  automationReady = new Promise((resolve) => { resolveAutomationReady = resolve; });
}

function refreshViews() {
  u8 = new Uint8Array(memory.buffer);
  u16 = new Uint16Array(memory.buffer);
}

function bytesToBase64(bytes) {
  let binary = '';
  for (let i = 0; i < bytes.length; i += 0x8000) {
    const chunk = bytes.subarray(i, i + 0x8000);
    binary += String.fromCharCode(...chunk);
  }
  return btoa(binary);
}

function base64ToBytes(base64) {
  const binary = atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

function flashBytes() {
  return u8.subarray(FLASH_BASE, FLASH_BASE + FLASH_SIZE);
}

function hashBytes(bytes) {
  let hash = 2166136261;
  for (const byte of bytes) {
    hash ^= byte;
    hash = Math.imul(hash, 16777619);
  }
  return hash >>> 0;
}

function loadFlashSave(sourceBytes) {
  const flash = flashBytes();
  flash.fill(0xFF);

  if (sourceBytes?.length === FLASH_SIZE) {
    flash.set(normalizeSaveForCurrentBuild(sourceBytes) ?? sourceBytes);
  } else {
    try {
      const stored = localStorage.getItem(SAVE_STORAGE_KEY);
      if (stored) {
        const saved = base64ToBytes(stored);
        if (saved.length === FLASH_SIZE) {
          const normalized = normalizeSaveForCurrentBuild(saved);
          flash.set(normalized ?? saved);
          if (normalized && normalized !== saved) localStorage.setItem(SAVE_STORAGE_KEY, bytesToBase64(normalized));
        }
      }
    } catch {
      // Storage may be disabled; the game can still run with volatile flash.
    }
  }

  lastSavedFlashHash = hashBytes(flash);
}

function saveFlashIfChanged(force = false) {
  if (!u8) return;
  const now = performance.now();
  if (!force && now - lastSaveFlushTime < SAVE_FLUSH_INTERVAL_MS) return;
  lastSaveFlushTime = now;

  const flash = flashBytes();
  const hash = hashBytes(flash);
  if (!force && hash === lastSavedFlashHash) return;

  try {
    localStorage.setItem(SAVE_STORAGE_KEY, bytesToBase64(flash));
    lastSavedFlashHash = hash;
  } catch {
    // Keep running even if the browser refuses persistent storage.
  }
}

function downloadSave() {
  if (!u8) return;
  saveFlashIfChanged(true);
  const blob = new Blob([new Uint8Array(flashBytes())], { type: 'application/octet-stream' });
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = 'pokeemerald.sav';
  document.body.append(link);
  link.click();
  link.remove();
  URL.revokeObjectURL(url);
}

function readSaveU16(bytes, offset) {
  return bytes[offset] | (bytes[offset + 1] << 8);
}

function readSaveU32(bytes, offset) {
  return (bytes[offset]
    | (bytes[offset + 1] << 8)
    | (bytes[offset + 2] << 16)
    | (bytes[offset + 3] << 24)) >>> 0;
}

function writeSaveU16(bytes, offset, value) {
  bytes[offset] = value & 0xff;
  bytes[offset + 1] = (value >> 8) & 0xff;
}

function saveSectorChecksum(bytes, offset, size) {
  let sum = 0;
  let i = 0;
  for (; i + 3 < size; i += 4) sum = (sum + readSaveU32(bytes, offset + i)) >>> 0;
  for (; i < size; i++) sum = (sum + (bytes[offset + i] << ((i & 3) * 8))) >>> 0;
  return ((sum >>> 16) + (sum & 0xffff)) & 0xffff;
}

function hasValidEmeraldSaveSlot(bytes, slot, sectorDataSizes) {
  let validSectorFlags = 0;
  const firstSector = slot * SAVE_SECTORS_PER_SLOT;

  for (let i = 0; i < SAVE_SECTORS_PER_SLOT; i++) {
    const sectorOffset = (firstSector + i) * FLASH_SECTOR_SIZE;
    const id = readSaveU16(bytes, sectorOffset + 0x0ff4);
    const checksum = readSaveU16(bytes, sectorOffset + 0x0ff6);
    const signature = readSaveU32(bytes, sectorOffset + 0x0ff8);
    const dataSize = sectorDataSizes[id];

    if (signature === SAVE_SECTOR_SIGNATURE
      && dataSize !== undefined
      && checksum === saveSectorChecksum(bytes, sectorOffset, dataSize)) {
      validSectorFlags |= 1 << id;
    }
  }

  return validSectorFlags === (1 << SAVE_SECTORS_PER_SLOT) - 1;
}

function isValidEmeraldSave(bytes, sectorDataSizes) {
  return hasValidEmeraldSaveSlot(bytes, 0, sectorDataSizes)
    || hasValidEmeraldSaveSlot(bytes, 1, sectorDataSizes);
}

function isValidCurrentBuildSave(bytes) {
  return isValidEmeraldSave(bytes, SAVE_SECTOR_DATA_SIZES)
    || isValidEmeraldSave(bytes, VANILLA_SAVE_SECTOR_DATA_SIZES);
}

function copyRange(dst, dstOffset, src, srcOffset, size) {
  dst.set(src.subarray(srcOffset, srcOffset + size), dstOffset);
}

function copyLegacyArray(dst, dstOffset, dstElementSize, src, srcOffset, srcElementSize, count) {
  for (let i = 0; i < count; i++) {
    copyRange(dst, dstOffset + i * dstElementSize, src, srcOffset + i * srcElementSize, srcElementSize);
  }
}

function readSlotSaveBlocks(bytes, slot, sectorDataSizes, saveBlock2Size, saveBlock1Size) {
  const saveBlock2 = new Uint8Array(saveBlock2Size);
  const saveBlock1 = new Uint8Array(saveBlock1Size);
  const firstSector = slot * SAVE_SECTORS_PER_SLOT;

  for (let i = 0; i < SAVE_SECTORS_PER_SLOT; i++) {
    const sectorOffset = (firstSector + i) * FLASH_SECTOR_SIZE;
    const id = readSaveU16(bytes, sectorOffset + 0x0ff4);
    const size = sectorDataSizes[id];
    if (size === undefined) continue;

    if (id === 0) {
      copyRange(saveBlock2, 0, bytes, sectorOffset, Math.min(size, saveBlock2.length));
    } else if (id >= 1 && id <= 4) {
      copyRange(saveBlock1, (id - 1) * 0x0f80, bytes, sectorOffset, Math.min(size, saveBlock1.length - (id - 1) * 0x0f80));
    }
  }

  return { saveBlock1, saveBlock2 };
}

function saveKeyScore(saveBlock1, key) {
  let score = (readSaveU32(saveBlock1, 0x490) ^ key) <= 999999 ? 20 : -20;

  for (const [offset, count, maxQuantity] of SAVE_BAG_POCKETS) {
    for (let i = 0; i < count; i++) {
      const itemOffset = offset + i * 4;
      const itemId = readSaveU16(saveBlock1, itemOffset);
      const quantity = (readSaveU16(saveBlock1, itemOffset + 2) ^ key) & 0xffff;
      if (itemId === 0) score += quantity === 0 ? 1 : -1;
      else if (itemId < 377 && quantity > 0 && quantity <= maxQuantity) score += 3;
      else score -= 3;
    }
  }

  return score;
}

function isLikelyLegacyWasmSaveBlock(blocks) {
  const legacyKey = readSaveU32(blocks.saveBlock2, 0x0a8);
  const currentKey = readSaveU32(blocks.saveBlock2, SAVE_BLOCK2_ENCRYPTION_KEY_OFFSET);
  if (legacyKey === currentKey) return false;
  return saveKeyScore(blocks.saveBlock1, legacyKey) > saveKeyScore(blocks.saveBlock1, currentKey) + 20;
}

function isPlausibleBagQuantity(itemId, quantity, maxQuantity) {
  if (itemId === 0) return quantity === 0;
  return itemId < 377 && quantity > 0 && quantity <= maxQuantity;
}

function bagQuantityKeyScore(saveBlock1, key) {
  let score = 0;
  const keyLow = key & 0xffff;

  for (const [offset, count, maxQuantity] of SAVE_BAG_POCKETS) {
    for (let i = 0; i < count; i++) {
      const itemOffset = offset + i * 4;
      const itemId = readSaveU16(saveBlock1, itemOffset);
      const quantity = (readSaveU16(saveBlock1, itemOffset + 2) ^ keyLow) & 0xffff;
      if (isPlausibleBagQuantity(itemId, quantity, maxQuantity)) score += itemId === 0 ? 2 : 5;
      else score += itemId === 0 ? -2 : -5;
    }
  }

  return score;
}

function staleBagQuantityKey(saveBlock1, currentKey) {
  const currentKeyLow = currentKey & 0xffff;
  const candidates = new Set();

  for (const [offset, count] of SAVE_BAG_POCKETS) {
    for (let i = 0; i < count; i++) {
      const itemOffset = offset + i * 4;
      if (readSaveU16(saveBlock1, itemOffset) === 0) {
        const rawQuantity = readSaveU16(saveBlock1, itemOffset + 2);
        if (rawQuantity !== currentKeyLow) candidates.add(rawQuantity);
      }
    }
  }

  const currentScore = bagQuantityKeyScore(saveBlock1, currentKeyLow);
  let bestKey = currentKeyLow;
  let bestScore = currentScore;
  for (const candidate of candidates) {
    const score = bagQuantityKeyScore(saveBlock1, candidate);
    if (score > bestScore) {
      bestKey = candidate;
      bestScore = score;
    }
  }

  return bestKey !== currentKeyLow && bestScore > currentScore + 100 && bestScore > 100 ? bestKey : null;
}

function reencryptSaveBlock1Hword(saveBlock1, offset, oldKey, newKey) {
  const value = (readSaveU16(saveBlock1, offset) ^ oldKey) & 0xffff;
  writeSaveU16(saveBlock1, offset, value ^ newKey);
}

function repairStaleBagEncryption(blocks) {
  const currentKey = readSaveU32(blocks.saveBlock2, SAVE_BLOCK2_ENCRYPTION_KEY_OFFSET);
  const currentKeyLow = currentKey & 0xffff;
  const oldKey = staleBagQuantityKey(blocks.saveBlock1, currentKey);
  if (oldKey === null) return null;

  const saveBlock1 = new Uint8Array(blocks.saveBlock1);
  let didRepair = false;
  for (const [offset, count, maxQuantity] of SAVE_BAG_POCKETS) {
    for (let i = 0; i < count; i++) {
      const quantityOffset = offset + i * 4 + 2;
      const itemId = readSaveU16(saveBlock1, quantityOffset - 2);
      const rawQuantity = readSaveU16(saveBlock1, quantityOffset);
      const currentQuantity = (rawQuantity ^ currentKeyLow) & 0xffff;
      const oldQuantity = (rawQuantity ^ oldKey) & 0xffff;
      if (isPlausibleBagQuantity(itemId, currentQuantity, maxQuantity)
          || !isPlausibleBagQuantity(itemId, oldQuantity, maxQuantity)) {
        continue;
      }

      reencryptSaveBlock1Hword(saveBlock1, quantityOffset, oldKey, currentKeyLow);
      didRepair = true;
    }
  }

  const rawCoins = readSaveU16(saveBlock1, SAVE_BLOCK1_COINS_OFFSET);
  const currentCoins = (rawCoins ^ currentKeyLow) & 0xffff;
  const oldCoins = (rawCoins ^ oldKey) & 0xffff;
  if (oldCoins <= 9999 && (currentCoins > 9999 || rawCoins === oldKey)) {
    writeSaveU16(saveBlock1, SAVE_BLOCK1_COINS_OFFSET, oldCoins ^ currentKeyLow);
    didRepair = true;
  }

  return didRepair ? { saveBlock1, saveBlock2: blocks.saveBlock2 } : null;
}

function migrateLegacySaveBlock2(src) {
  const dst = new Uint8Array(0x0f2c);
  copyRange(dst, 0, src, 0, 0x98);
  copyRange(dst, 0x98, src, 0x98, 5);
  copyRange(dst, 0xa0, src, 0x9e, 5);
  copyRange(dst, 0xa8, src, 0xa4, 0xe64);
  return dst;
}

function migrateLegacySaveBlock1(src) {
  const dst = new Uint8Array(0x3d8c);
  copyRange(dst, 0, src, 0, 0x848);
  copyLegacyArray(dst, 0x848, 8, src, 0x848, 7, 40);
  copyRange(dst, 0x988, src, 0x960, 0x166c - 0x960);
  copyLegacyArray(dst, 0x169c, 8, src, 0x1674, 6, 128);
  copyRange(dst, 0x1a9c, src, 0x1974, 0x2be0 - 0x1974);
  copyLegacyArray(dst, 0x2be0, 36, src, 0x2ab8, 34, 16);
  copyRange(dst, 0x2e20, src, 0x2cd8, 0x3150 - 0x2cd8);
  copyRange(dst, 0x3150, src, 0x3008, 85);
  copyRange(dst, 0x31a8, src, 0x305e, 11);
  copyRange(dst, 0x31b4, src, 0x306c, 20);
  copyRange(dst, 0x31c8, src, 0x3080, 21);
  copyRange(dst, 0x31e0, src, 0x3098, 0x3c44 - 0x3098);
  return dst;
}

function writeCurrentSaveSlot(out, source, slot, currentBlocks) {
  const firstSector = slot * SAVE_SECTORS_PER_SLOT;

  for (let i = 0; i < SAVE_SECTORS_PER_SLOT; i++) {
    const sectorOffset = (firstSector + i) * FLASH_SECTOR_SIZE;
    const id = readSaveU16(source, sectorOffset + 0x0ff4);
    const size = SAVE_SECTOR_DATA_SIZES[id];
    if (size === undefined) continue;

    out.fill(0, sectorOffset, sectorOffset + 0x0ff4);
    if (id === 0) {
      copyRange(out, sectorOffset, currentBlocks.saveBlock2, 0, size);
    } else if (id >= 1 && id <= 4) {
      copyRange(out, sectorOffset, currentBlocks.saveBlock1, (id - 1) * 0x0f80, size);
    } else {
      copyRange(out, sectorOffset, source, sectorOffset, size);
    }
    writeSaveU16(out, sectorOffset + 0x0ff6, saveSectorChecksum(out, sectorOffset, size));
  }
}

function writeMigratedSaveSlot(out, source, slot, blocks) {
  writeCurrentSaveSlot(out, source, slot, {
    saveBlock2: migrateLegacySaveBlock2(blocks.saveBlock2),
    saveBlock1: migrateLegacySaveBlock1(blocks.saveBlock1),
  });
}

function migrateLegacyWasmSave(bytes) {
  const out = new Uint8Array(bytes);
  let didMigrate = false;

  for (let slot = 0; slot < 2; slot++) {
    for (const legacySizes of [LEGACY_WASM_SAVE_SECTOR_DATA_SIZES, LEGACY_WASM_FRONTEND_SAVE_SECTOR_DATA_SIZES]) {
      if (!hasValidEmeraldSaveSlot(bytes, slot, legacySizes)) continue;
      const blocks = readSlotSaveBlocks(bytes, slot, legacySizes, LEGACY_SAVE_BLOCK2_SIZE, LEGACY_SAVE_BLOCK1_SIZE);
      if (!isLikelyLegacyWasmSaveBlock(blocks)) continue;
      writeMigratedSaveSlot(out, bytes, slot, blocks);
      didMigrate = true;
      break;
    }
  }

  return didMigrate ? out : null;
}

function repairStaleBagEncryptionSave(bytes) {
  const out = new Uint8Array(bytes);
  let didRepair = false;

  for (let slot = 0; slot < 2; slot++) {
    let sectorDataSizes = null;
    if (hasValidEmeraldSaveSlot(bytes, slot, SAVE_SECTOR_DATA_SIZES)) sectorDataSizes = SAVE_SECTOR_DATA_SIZES;
    else if (hasValidEmeraldSaveSlot(bytes, slot, VANILLA_SAVE_SECTOR_DATA_SIZES)) sectorDataSizes = VANILLA_SAVE_SECTOR_DATA_SIZES;
    if (!sectorDataSizes) continue;

    const blocks = readSlotSaveBlocks(bytes, slot, sectorDataSizes, SAVE_BLOCK2_SIZE, SAVE_BLOCK1_SIZE);
    const repaired = repairStaleBagEncryption(blocks);
    if (!repaired) continue;

    writeCurrentSaveSlot(out, bytes, slot, repaired);
    didRepair = true;
  }

  return didRepair ? out : null;
}

function normalizeSaveForCurrentBuild(bytes) {
  const migrated = migrateLegacyWasmSave(bytes);
  if (migrated) return repairStaleBagEncryptionSave(migrated) ?? migrated;

  const repaired = repairStaleBagEncryptionSave(bytes);
  if (repaired) return repaired;

  return isValidCurrentBuildSave(bytes) ? bytes : null;
}

async function wasmModule() {
  wasmModulePromise ??= fetch('/build/wasm/pokeemerald.wasm', { cache: 'no-store' })
    .then((res) => res.arrayBuffer())
    .then(async (bytes) => ({ bytes, module: await WebAssembly.compile(bytes) }));
  return wasmModulePromise;
}

async function restartWithSave(bytes) {
  statusText = 'restarting with uploaded save...';
  statusEl.textContent = statusText;
  pressed.clear();
  pendingPresses.clear();
  await boot(bytes);
}

async function uploadSave(file) {
  if (!file || !u8) return;
  const bytes = new Uint8Array(await file.arrayBuffer());
  if (bytes.length !== FLASH_SIZE) {
    statusEl.textContent = `expected a ${FLASH_SIZE} byte Emerald .sav file, got ${bytes.length} bytes`;
    return;
  }
  const normalized = normalizeSaveForCurrentBuild(bytes);
  if (!normalized) {
    statusEl.textContent = 'save file does not contain a valid Emerald save slot';
    return;
  }

  lastSavedFlashHash = hashBytes(normalized);
  localStorage.setItem(SAVE_STORAGE_KEY, bytesToBase64(normalized));
  await restartWithSave(normalized);
}

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function formatSpeed(value) {
  if (value === Infinity) return 'unlimited';
  if (value < 1) return `${value.toFixed(1)}x`;
  if (value < 100) return `${value.toFixed(value < 10 ? 1 : 0)}x`;
  return `${Math.round(value)}x`;
}

function setSpeed(value) {
  speed = value === Infinity ? Infinity : clamp(value, MIN_SPEED, MAX_SPEED);
  if (speed !== Infinity) lastFiniteSpeed = speed;
  speedValue.textContent = formatSpeed(speed);
  speedButtons.forEach((button) => {
    button.classList.toggle('active',
      (button.dataset.speed === '1' && speed === 1) || (button.dataset.speed === 'unlimited' && speed === Infinity));
  });
}

function adjustSpeed(multiplier) {
  setSpeed((speed === Infinity ? lastFiniteSpeed : speed) * multiplier);
}

function toggleUnlimitedSpeed() {
  setSpeed(speed === Infinity ? lastFiniteSpeed : Infinity);
}

function resetFpsCounters() {
  lastFpsUpdate = performance.now();
  renderedFrames = 0;
  emulatedFrames = 0;
  gameFrameAccumulator = 0;
}

function initialSpeed() {
  if (speedParam === '0') return Infinity;
  const parsed = Number(speedParam);
  return Number.isFinite(parsed) && parsed > 0 ? parsed : 1;
}

function updateFullscreenButton() {
  const isFullscreen = document.fullscreenElement === shell;
  fullscreenButton.textContent = isFullscreen ? 'Exit Big Picture' : 'Big Picture';
  fullscreenButton.classList.toggle('active', isFullscreen);
}

async function toggleFullscreen() {
  if (document.fullscreenElement) {
    await document.exitFullscreen();
  } else {
    await shell.requestFullscreen();
  }
}

function refreshFrameImage() {
  const ptr = instance.exports.WasmDisplayBuffer();
  const size = instance.exports.WasmDisplayBufferSize();
  image = new ImageData(new Uint8ClampedArray(memory.buffer, ptr, size), WIDTH, HEIGHT);
}

function render() {
  instance.exports.WasmRenderFrame();
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
  const angle = rotation * Math.PI * 2 / 0x10000;
  const sin = Math.sin(angle) * 256;
  const cos = Math.cos(angle) * 256;
  return {
    pa: cos * xScale / 256,
    pb: -sin * xScale / 256,
    pc: sin * yScale / 256,
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
    pendingPresses.set(name, 1);
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

window.addEventListener('beforeunload', () => saveFlashIfChanged(true));
window.addEventListener('pagehide', () => saveFlashIfChanged(true));
window.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'hidden') saveFlashIfChanged(true);
});

document.querySelectorAll('[data-key]').forEach((button) => {
  const name = button.dataset.key;
  button.addEventListener('pointerdown', (event) => { event.preventDefault(); setPressed(name, true); });
  button.addEventListener('pointerup', () => setPressed(name, false));
  button.addEventListener('pointercancel', () => setPressed(name, false));
  button.addEventListener('pointerleave', () => setPressed(name, false));
});

speedButtons.forEach((button) => {
  button.addEventListener('click', () => {
    if (button.dataset.speed === 'unlimited') toggleUnlimitedSpeed();
    else if (button.dataset.speed === '1') setSpeed(1);
    else adjustSpeed(Number(button.dataset.speed));
    resetFpsCounters();
  });
});

fullscreenButton.addEventListener('click', async () => {
  try {
    await toggleFullscreen();
  } catch (error) {
    console.error(error);
    statusEl.textContent = error.stack || String(error);
  }
});

document.addEventListener('fullscreenchange', updateFullscreenButton);
updateFullscreenButton();

downloadSaveButton.addEventListener('click', downloadSave);

uploadSaveInput.addEventListener('change', async () => {
  try {
    await uploadSave(uploadSaveInput.files[0]);
  } catch (error) {
    console.error(error);
    statusEl.textContent = error.stack || String(error);
  } finally {
    uploadSaveInput.value = '';
  }
});

function fpsStatus(displayFps, gameFps) {
  return `${statusText} — Display FPS: ${displayFps}, Game FPS: ${gameFps} (${(gameFps / 60).toFixed(1)}x)`;
}

async function boot(saveBytes) {
  const thisBootId = ++bootId;
  const { bytes, module } = await wasmModule();
  instance = await WebAssembly.instantiate(module, importsFor(module));
  memory = instance.exports.memory;
  window.pokeemerald = { instance, memory, runFrames };
  if (automate) window.pokeemerald.automation = automationApi();
  refreshViews();
  refreshFrameImage();
  loadFlashSave(saveBytes);
  writeKeys();
  instance.exports.AgbMain();
  currentFrame = 0;
  gameFrameAccumulator = 0;
  resetFpsCounters();
  statusText = `running — ${(bytes.byteLength / 1024 / 1024).toFixed(1)} MiB wasm`;
  setSpeed(initialSpeed());
  statusEl.textContent = fpsStatus(0, 0);
  if (automate) {
    render();
    resolveAutomationReady();
  } else {
    requestAnimationFrame((now) => tick(thisBootId, now));
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
  statusEl.textContent = fpsStatus(fps, gameFps);
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
  saveFlashIfChanged();
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
  return (u16[ptr >> 1] | (u16[(ptr + 2) >> 1] << 16)) >>> 0;
}

function hblankDmaWin0HProbe() {
  const src = 0x0203ff00;
  const dma = REG + REG_OFFSET_DMA0;
  const callDmaStop0 = () => {
    if (typeof instance.exports.WasmDmaStop0 !== 'function') throw new Error('WasmDmaStop0 export unavailable');
    instance.exports.WasmDmaStop0();
  };
  const probeMode = (destMode) => {
    writeS32(dma, src);
    writeS32(dma + 4, REG + REG_OFFSET_WIN0H);
    writeS32(dma + 8, 1 | ((DMA_ENABLE | DMA_START_HBLANK | DMA_REPEAT | destMode) << 16));

    instance.exports.WasmRefreshHblankDmaGpuRegs();
    const activeLine0 = instance.exports.WasmWindowMask(5, 0);
    const activeLine1 = instance.exports.WasmWindowMask(5, 1);
    const activeLine2 = instance.exports.WasmWindowMask(5, 2);
    const activeCached = Boolean(instance.exports.WasmHblankDmaGpuRegActive(REG_OFFSET_WIN0H));

    callDmaStop0();
    const stoppedControl = u16[(dma + 10) >> 1];
    instance.exports.WasmRefreshHblankDmaGpuRegs();
    const stoppedLine0 = instance.exports.WasmWindowMask(5, 0);
    const stoppedLine1 = instance.exports.WasmWindowMask(5, 1);
    const stoppedLine2 = instance.exports.WasmWindowMask(5, 2);

    return {
      activeLine0,
      activeLine1,
      activeLine2,
      activeCached,
      stoppedControl,
      stoppedLine0,
      stoppedLine1,
      stoppedLine2,
      stoppedCached: Boolean(instance.exports.WasmHblankDmaGpuRegActive(REG_OFFSET_WIN0H)),
    };
  };
  const saved = {
    dispcnt: u16[REG >> 1],
    win0h: u16[(REG + REG_OFFSET_WIN0H) >> 1],
    win0v: u16[(REG + REG_OFFSET_WIN0V) >> 1],
    winin: u16[(REG + REG_OFFSET_WININ) >> 1],
    winout: u16[(REG + REG_OFFSET_WINOUT) >> 1],
    src0: u16[src >> 1],
    src1: u16[(src + 2) >> 1],
    dmaSrc: readU32(dma),
    dmaDest: readU32(dma + 4),
    dmaControl: readU32(dma + 8),
  };
  let result;

  try {
    u16[REG >> 1] = 0x2000;
    u16[(REG + REG_OFFSET_WIN0H) >> 1] = (20 << 8) | 30;
    u16[(REG + REG_OFFSET_WIN0V) >> 1] = HEIGHT;
    u16[(REG + REG_OFFSET_WININ) >> 1] = 0x3f;
    u16[(REG + REG_OFFSET_WINOUT) >> 1] = 0;
    u16[src >> 1] = 10;
    u16[(src + 2) >> 1] = (20 << 8) | 30;

    result = {
      fixed: probeMode(DMA_DEST_FIXED),
      reload: probeMode(DMA_DEST_RELOAD),
    };
  } finally {
    u16[REG >> 1] = saved.dispcnt;
    u16[(REG + REG_OFFSET_WIN0H) >> 1] = saved.win0h;
    u16[(REG + REG_OFFSET_WIN0V) >> 1] = saved.win0v;
    u16[(REG + REG_OFFSET_WININ) >> 1] = saved.winin;
    u16[(REG + REG_OFFSET_WINOUT) >> 1] = saved.winout;
    u16[src >> 1] = saved.src0;
    u16[(src + 2) >> 1] = saved.src1;
    writeS32(dma, saved.dmaSrc);
    writeS32(dma + 4, saved.dmaDest);
    writeS32(dma + 8, saved.dmaControl);
    instance.exports.WasmRefreshHblankDmaGpuRegs();
  }

  return result;
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
    birchLabState: instance.exports.VarGet(0x4084),
    littlerootRivalState: instance.exports.VarGet(0x408d),
    littlerootIntroState: instance.exports.VarGet(0x4092),
    oldaleRivalState: instance.exports.VarGet(0x40c7),
    starterMon: instance.exports.VarGet(0x4023),
  };
}

function automationApi() {
  return {
    ready: automationReady,
    setButton: setAutomationButton,
    runToFrame,
    screenshot: () => canvas.toDataURL('image/png'),
    hblankDmaWin0HProbe,
    state: automationState,
    frame: () => currentFrame,
  };
}

function runFramesForTick(elapsedMs) {
  const start = performance.now();
  const frameBudgetMs = speed === Infinity ? elapsedMs : FAST_FRAME_BUDGET_MS;
  let frameCount;

  if (speed === Infinity) {
    gameFrameAccumulator = 0;
    frameCount = Infinity;
  } else {
    gameFrameAccumulator += speed * elapsedMs / (1000 / 60);
    frameCount = Math.floor(gameFrameAccumulator);
    gameFrameAccumulator -= frameCount;
    if (frameCount === 0) return 0;
  }

  let frames = 0;
  while (frames < frameCount && performance.now() - start < frameBudgetMs) {
    const batchSize = Math.min(frameCount - frames, 256);
    runFrames(batchSize);
    frames += batchSize;
  }
  return frames;
}

function tick(thisBootId, now) {
  if (thisBootId !== bootId) return;
  try {
    const elapsedMs = Math.min(now - lastTick, 100);
    lastTick = now;
    const frames = runFramesForTick(elapsedMs);
    render();
    updateFps(frames);
    requestAnimationFrame((nextNow) => tick(thisBootId, nextNow));
  } catch (error) {
    console.error(error);
    statusEl.textContent = error.stack || String(error);
  }
}

boot().catch((error) => {
  console.error(error);
  statusEl.textContent = error.stack || String(error);
});
