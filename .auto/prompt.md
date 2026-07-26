# Autoresearch: full-scope pokeemerald performance (equivalent logic/gameplay)

## Objective
Increase the headless native engine's `WasmRunFrame` throughput (FPS) for the
pokeemerald WASM-to-native port, over the full project scope, while preserving
EXACT game logic and gameplay. The game runs as a wasm2c-translated module; the
timed work is real game logic (overworld/menu/battle), not rendering.

The benchmark replays `tools/wasm_replays/mudkip_starter.txt` from a blank save
through three scenarios (overworld, menu, battle). Each scenario warms 10,007
frames then times exactly 1,000,003 `WasmRunFrame` calls; rendering is outside
the timed region. Score = arithmetic mean of the three aggregate scenario FPS.

## Metrics
- **Primary**: `fps` (frames/sec, HIGHER is better) — mean aggregate across scenarios.
- **Secondary**: `overworld_fps`, `menu_fps`, `battle_fps` — locate which scenario
  a change helps/hurts. Overworld and battle are the slow ones (~1.6M); menu is
  fast (~2.6M).

## How to Run
`./.auto/measure.sh` — builds `native-bench` in the current dir, runs the bench
with 3 internal passes, and emits `METRIC fps=... ` plus per-scenario metrics.
It HARD-FAILS (exit 1) on any golden-hash mismatch, nondeterminism, or
non-progressing run. `.auto/checks.sh` additionally rejects any candidate that
modified the immutable oracle files and re-verifies the equivalence gate.

## The correctness oracle (already strong — do NOT weaken)
The benchmark is not loose checkpoints: it is three full 1,000,003-frame
DETERMINISTIC trajectories pinned by exact start/end framebuffer hashes in
`tools/native/bench_golden.json`. Any logic divergence accumulates over 1M
frames and changes the end hash -> fail. So preserving the hashes == preserving
real gameplay along those trajectories. This is the anti-overfit backbone.

## ANTI-CHEAT / ANTI-OVERFIT RULES (hard)
- NEVER modify or generate: `tools/native/bench_main.c`, `tools/native/run_bench.sh`,
  `tools/native/bench_golden.json`, `tools/wasm_replays/*`, `.auto/*`. checks.sh
  rejects these.
- No scenario/frame special-casing, no hardcoded hashes, no skipping work that
  changes rendered output, no nondeterminism, no undefined behavior.
- A legit optimization removes genuinely redundant work or produces
  faster-but-identical code. If output is identical, the hashes pass legitimately.
- Prefer GENERAL, provably-equivalent refactors over clever tricks. Simpler is better.
- Display/renderer code (`src/wasm_display.c`, `WasmRenderFrame`) is OUTSIDE the
  timed region: optimizing it does NOT move the metric. Don't waste arms on it.

## Files in Scope
- `src/*.c` — decompiled game logic compiled to WASM. THE main optimization
  surface (the timed work lives here). Changes here affect ALL builds, so keep
  them provably behavior-preserving; guard with `#if WASM` only if behavior must
  differ (prefer guard-free equivalence).
- `include/**/*.h` — headers; a change forces a full wasm rebuild (slow).
- `tools/native/native_engine.c/.h` — host BIOS/syscall shims. Profiling shows
  these are NOT the bottleneck; low value.
- `tools/native/performance.mk` — native/wasm compiler flags. Already tuned
  (-O3 -flto native, -O2 wasm); flag tweaks are within noise (see tried).
- `Makefile` — build rules. The native-bench wiring is off-limits in spirit.

## Off Limits
The oracle files above; `data/` and `graphics/` (assets, not logic); the
generated `build/` tree; original gameplay semantics.

## Constraints
- No ARM toolchain in this env: `make modern`/`make compare` (matching GBA ROM)
  CANNOT be built here. So shared-source changes must be obviously build-agnostic
  pure C (same algorithm, same result) or WASM-guarded. Do not rely on compiling
  the GBA path.
- `make wasm` and `make native-bench` must keep working.
- No new dependencies.

## Profiling findings (macOS `sample` on the bench, 60 passes)
Hot spots as a fraction of total sampled frames:
- **BuildOamBuffer ~50%** (src/sprite.c) — BY FAR the biggest. Runs every frame
  in the VBlank path. Contains: UpdateOamCoords, BuildSpritePriorities,
  SortSprites (O(n^2) insertion sort over MAX_SPRITES=64), AddSpritesToOamBuffer,
  CopyMatricesToOamBuffer.
- AnimateSprites ~13% (sprite callbacks + AnimateSprite).
- Affine anims ~8% (ContinueAffineAnim, AffineAnimCmd_end, BeginAffineAnim).
- VBlankIntr ~3%, CB1_Overworld ~2%, TransferPlttBuffer ~2%,
  UpdateTilesetAnimations, UpdateObjectEventCurrentMovement, HandleLinkConnection,
  RunTextPrinters, CameraUpdate — each ~1%.
Focus arms on BuildOamBuffer / sprite.c first; it dominates.

## What's Been Tried (from prior GEPA optimize-anything runs — do NOT repeat)
- Native compiler flags: `-O3 -DNDEBUG -fomit-frame-pointer -flto` (native) and
  linker `-flto` are the proven best; already on master. `-mcpu=native`, ThinLTO,
  native `-O2` were worse/neutral.
- `WASM_OPT_FLAGS=-O3` (vs -O2): neutral/within noise.
- `-fno-stack-protector -fno-semantic-interposition`: A/B tested, NO help
  (baseline 1.994M vs flags 1.972M).
- native_engine.c host-shim micro-opts (bulk copy_units, etc.): no candidate beat
  the seed on full validation. Host shims are not the bottleneck.
- Baseline noise band is ~1.96M-2.04M FPS (run-to-run +/-~2.5%). Wins must clear
  this; trust the confidence score and re-verify winners with a clean rebuild.

## Winner handling
`resolve_iteration` auto-commits the best improving arm. Treat committed winners
as PROVISIONAL: re-verify with a clean full rebuild + multiple bench runs before
trusting a large jump; revert if it does not reproduce (likely noise/stale build).
