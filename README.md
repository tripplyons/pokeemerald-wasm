# pokeemerald-wasm

pokeemerald-wasm is a recompilation of the original [pret/pokeemerald](https://github.com/pret/pokeemerald) decompilation to WebAssembly, with a browser frontend for running Pokémon Emerald on the web.

Play it at [pokeemerald.com](https://pokeemerald.com).

## Native Raylib frontend

`make native-raylib` builds `build/native/pokeemerald-native` by first building the WASM module, converting it to C with WABT's `wasm2c`, and linking a Raylib host app. The native app uses the same WASM ABI as the browser frontend and stores flash saves in `build/native/pokeemerald-native.sav`.

## Kindle Scribe frontend

`make native-kindle` builds `build/native/pokeemerald-kindle`, a Linux framebuffer frontend intended for jailbroken Kindle Scribe devices. It uses the same wasm2c game core, writes grayscale frames directly to `/dev/fb0`, reads touch/keyboard events from `/dev/input/event*`, draws on-screen GBA controls, refreshes e-ink through HWTCON/MXCFB or `eips` when available, and stores flash saves in `build/native/pokeemerald-kindle.sav`.

For real Scribe deployment, cross-compile with a Kindle-compatible Linux toolchain, for example `make native-kindle KINDLE_CC='zig cc' KINDLE_CFLAGS='-target arm-linux-musleabihf -O2 -DNDEBUG -static'`. The Kindle target compiles WABT's `wasm-rt` sources for the selected target instead of linking the host WABT library. Runtime options include `--fb /dev/fb0`, `--input /dev/input/eventN`, `--display-fps 4`, `--save path`, and `--frames N`.

The original pokeemerald README has been preserved at [docs/original-pokeemerald-readme.md](docs/original-pokeemerald-readme.md).
