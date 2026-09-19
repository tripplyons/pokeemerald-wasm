# Tunable flags for the direct native engine. This file is the intended
# compile/link configuration surface for native performance work.
# `make native-pgo` trains NATIVE_PGO_PROFILE on the benchmark replay. Desktop
# builds use the profile while it exists; `make clean-native` removes it.
NATIVE_PGO_PROFILE ?= $(BUILD_DIR)/native/pgo/native.profdata
NATIVE_PGO_FLAGS ?= $(if $(wildcard $(NATIVE_PGO_PROFILE)),-fprofile-use=$(NATIVE_PGO_PROFILE))
NATIVE_CFLAGS ?= -O3 -DNDEBUG -fomit-frame-pointer -flto $(NATIVE_PGO_FLAGS)
NATIVE_LDFLAGS ?= -flto
WASM_OPT_FLAGS ?= -O2
WASM_LDFLAGS ?=
