# Agent Instructions

This repo is a pokeemerald decompilation with an in-progress WebAssembly port.
The primary goal is to run the game in the browser while preserving the original
source tree and behavior as much as possible.

## Project Direction

- Prefer using the original pokeemerald source, data, build rules, and asset
  pipeline directly.
- Avoid modifying original gameplay/source/data files when a WASM-only wrapper,
  generated file, browser shim, or build rule can solve the problem.
- When a change to original source is necessary for the WASM port, keep it as
  small as possible and guard behavioral differences with `#if WASM` or
  `#ifdef WASM`.
- Do not change non-WASM behavior unless the user explicitly asks for it.
- Keep the normal GBA build path meaningful. WASM work should not casually break
  `make`, `make modern`, or `make compare`.

## WASM Port Layout

- `make wasm` builds `build/wasm/pokeemerald.wasm`.
- `make serve-wasm` builds the WASM target and serves the browser frontend.
- Browser runtime code lives in `web/`.
- The desktop/native port lives in `tools/native/`:
  - `native_engine.h` / `native_engine.c`: headless engine core (wasm2c
    instance, `w2c_env_*` BIOS shims, pad input, flash saves, framebuffer
    access). No raylib dependency.
  - `raylib_main.c`: thin raylib GUI frontend (`make native-raylib`).
  - `bench_main.c`: headless FPS + correctness benchmark
    (`make native-bench`), never opens a window.
- WASM include shims live in `include/wasm/`.
- WASM generators and conversion tools live under `tools/`, for example
  `generate_wasm_assets.py`, `generate_wasm_text.py`, and `wasm_asm_data.py`.
- WASM-specific source/data that cannot come directly from upstream data should
  live under a clearly named WASM path such as `data/wasm/`.

## Implementation Rules

- Prefer adding WASM adapters at the boundary:
  - build rules in `Makefile`
  - browser glue in `web/`
  - compatibility headers in `include/wasm/`
  - generated C/asm/data under `build/wasm/`
  - source generators in `tools/`
- If original C files need WASM-specific behavior, isolate the exact difference
  with `#if WASM` near the affected code.
- Avoid broad rewrites of game logic to make the browser port easier.
- Avoid duplicating large source/data files. Generate translated forms when
  possible.
- Keep generated outputs out of git. Check `.gitignore` before committing if new
  build artifacts, screenshots, wasm files, or temporary data are created.
- The root `.gitignore` already ignores `build/`, `*.o`, `*.elf`, `*.gba`,
  `*.map`, `*.sym`, and most generated asset extensions. Add focused ignore
  rules if new generated artifacts fall outside those patterns.
- For every crash or gameplay bug, first identify the root cause and fix the
  shared conversion/runtime/build layer that caused it. Do not patch individual
  maps, events, cutscenes, battles, moves, or assets unless the root cause truly
  is isolated to that one piece of original content.
- Prefer fixes that apply automatically to whole classes of equivalent native
  data, commands, assets, or runtime paths. When a bug presents in one example,
  check for the underlying pattern and verify at least one representative path
  instead of hardcoding that example.
- After each verified change, commit the completed work before starting the next
  fix or feature, unless the user explicitly asks to keep changes uncommitted.

## Reproducible WASM Replay

Use `tools/wasm_replay.mjs` to run the normal `web/` frontend in a headless
Chrome/Chromium instance without the animation-frame cap and save deterministic
canvas screenshots, browser console output, and errors.

```sh
node tools/wasm_replay.mjs path/to/events.txt
```

The tool runs `make wasm`, starts `web/server.mjs`, opens `/?automate=1`, applies
input events at exact emulated game frame numbers, recreates the fixed
`wasm-replay-output/` folder, and writes:

- `screenshots/*.png` for requested screenshot frames
- `console.log` for server, browser, and page console messages
- `errors.log` for CLI or page exceptions
- `events.json` and `summary.json` for run metadata

Pass `--no-build` to reuse an existing `build/wasm/pokeemerald.wasm`. Set
`CHROME_BIN=/path/to/chrome` if Chrome/Chromium is not in a standard location.
The replay event file format is line oriented. Frame numbers are calls to
`WasmRunFrame()` / emulated game frames, not browser display frames:

```text
0 screenshot boot
120 button start on
124 button start off
240 screenshot title-screen
```

Valid buttons are `a`, `b`, `select`, `start`, `right`, `left`, `up`, `down`,
`r`, and `l`. `#` starts a comment. Keep reusable replay inputs in
`tools/wasm_replays/` and add screenshot events at meaningful route checkpoints
so future runs show where the automation is.

## Native Engine Benchmark

`make native-bench` builds `build/native/pokeemerald-bench`, a headless
benchmark with no GUI. It replays `tools/wasm_replays/mudkip_starter.txt`
from a blank save into separate overworld, menu, and battle states. Each
scenario warms up for 10,007 frames, then measures exactly 1,000,003
`WasmRunFrame` calls with no rendering inside the timed region. Rendering
before and after that fixed count produces FNV-hashed RGBA correctness
checkpoints without charging the software renderer to engine FPS.
`tools/native/run_bench.sh` builds and runs two passes against the golden
hashes in `tools/native/bench_golden.json`, printing one JSON object
`{"score": <mean aggregate scenario FPS>, "info": {...}}`; score is 0 on
any hash mismatch, nondeterminism, or invalid timer sample. Regenerate
goldens only after an intentional engine/rendering change:

```sh
./build/native/pokeemerald-bench --script tools/wasm_replays/mudkip_starter.txt --passes 2 --write-golden tools/native/bench_golden.json
```

## Build And Verification

For WASM changes, run:

```sh
make wasm
```

For browser/runtime changes, also run:

```sh
make serve-wasm
```

Then open the served page and verify the game reaches the intended flow.

When touching original source or shared build rules, also consider:

```sh
make modern
```

If the change could affect the matching ROM path, run:

```sh
make compare
```

Use the narrowest verification that gives real confidence, and report any tests
or builds that could not be run.

## Commit Guidance

- Follow the style of recent commits. Current WASM commits use short imperative
  subjects such as `Fix wasm window rendering` and `Add wasm FPS counter`.
- If asked to commit, inspect staged files before committing.
- Before committing, update `.gitignore` files if any generated or local-only
  artifacts should not be committed.
- Do not revert or overwrite uncommitted user changes unless the user explicitly
  asks.

## When Stuck

If you get stuck, spawn a subagent to answer the specific question blocking the
work. Keep the question narrow and include the relevant files, commands, and
failure output.
