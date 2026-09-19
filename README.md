# pokeemerald-wasm

pokeemerald-wasm is a recompilation of the original [pret/pokeemerald](https://github.com/pret/pokeemerald) decompilation to WebAssembly, with a browser frontend for running Pokémon Emerald on the web.

Play it at [pokeemerald.com](https://pokeemerald.com).

[Join our Discord server](https://discord.gg/u24yh5b83N)

For the 3D renderer built from this port, see [pokeemerald-3d](https://github.com/tripplyons/pokeemerald-3d).

## Native Raylib frontend

`make native-raylib` compiles the game C sources directly into `build/native/pokeemerald-native` and links the Raylib frontend. It supports macOS on Apple Silicon and Linux. Install Clang, Python 3, and Raylib (discoverable through `pkg-config`), along with the shared asset build dependencies. No WebAssembly module or WABT runtime is used. Flash saves are stored in `build/native/pokeemerald-native.sav`.

The native build reuses the browser port's C implementations of rendering and hardware operations. Generated native data keeps script addresses at 32 bits while adapting C pointer tables for the target architecture. `make native-bench` builds the same engine with the headless replay benchmark. `make native-test` checks native pointer translation and the flash-save round trip.

## Kindle Scribe frontend

`make native-kindle` builds `build/native/pokeemerald-kindle`, a Linux framebuffer frontend intended for jailbroken Kindle Scribe devices. It uses the same directly compiled native game core, writes grayscale frames directly to `/dev/fb0`, reads touch/keyboard events from `/dev/input/event*`, draws on-screen GBA controls with a `MAX SPEED` toggle that runs the engine uncapped, shows measured internal and display FPS under the game screen, refreshes e-ink through HWTCON/MXCFB or `eips` when available, and stores flash saves in `build/native/pokeemerald-kindle.sav`.

The default Kindle build uses Zig to cross-compile a static ARM Linux executable. Install Zig, or select a Kindle-compatible Clang toolchain with `KINDLE_CC` and `KINDLE_CFLAGS`. For example: `make native-kindle KINDLE_CC='zig cc' KINDLE_CFLAGS='-target arm-linux-musleabihf -mcpu=cortex_a7 -O2 -DNDEBUG -static'`. The Kindle target builds its game objects separately under `build/native/kindle/`; desktop objects are kept under `build/native/`. Clang is also used to analyze the preprocessed C with the cross compiler's target layout. To install for KUAL, copy `tools/kindle_kual/config.xml`, `tools/kindle_kual/menu.json`, `tools/kindle_kual/bin/start.sh`, and the built binary as `bin/pokeemerald-kindle` into `/mnt/us/extensions/pokeemerald/`. Runtime options include `--fb /dev/fb0`, `--input /dev/input/eventN`, `--display-fps 4`, `--save path`, and `--frames N`.

The original pokeemerald README has been preserved at [docs/original-pokeemerald-readme.md](docs/original-pokeemerald-readme.md).
