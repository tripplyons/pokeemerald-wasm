# pokeemerald-wasm

pokeemerald-wasm is a recompilation of the original [pret/pokeemerald](https://github.com/pret/pokeemerald) decompilation to WebAssembly, with a browser frontend for running Pokémon Emerald on the web.

Play it at [pokeemerald.com](https://pokeemerald.com).

## Native Raylib frontend

`make native-raylib` builds `build/native/pokeemerald-native` by first building the WASM module, converting it to C with WABT's `wasm2c`, and linking a Raylib host app. The native app uses the same WASM ABI as the browser frontend: it writes GBA input to `KEYINPUT`, advances `WasmRunFrame()`, renders `WasmRenderFrame()` into the exported 240x160 RGBA display buffer, uploads that buffer to a Raylib texture, draws input buttons, 0.5x/reset/2x speed controls, internal/display FPS counters, limits display refresh to realtime, budgets internal frame work to keep display updates near at least 6 FPS, and stores flash saves in `build/native/pokeemerald-native.sav`.

The original pokeemerald README has been preserved at [docs/original-pokeemerald-readme.md](docs/original-pokeemerald-readme.md).
