# Tunable flags for the desktop wasm2c engine. This file is the intended
# compile/link configuration surface for native performance work.
NATIVE_CFLAGS ?= -O2 -DNDEBUG
NATIVE_LDFLAGS ?=
WASM_OPT_FLAGS ?= -O2
WASM_LDFLAGS ?=
