# Tunable flags for the desktop wasm2c engine. This file is the intended
# compile/link configuration surface for native performance work.
NATIVE_CFLAGS ?= -O3 -DNDEBUG -fomit-frame-pointer -flto
NATIVE_LDFLAGS ?= -flto
WASM_OPT_FLAGS ?= -O2
WASM_LDFLAGS ?=
