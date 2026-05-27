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
