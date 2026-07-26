#!/bin/sh
# Official benchmark command for the native-engine optimize-anything
# profile. Builds the headless bench target (no window, no raylib) and
# runs the mudkip starter replay against the golden framebuffer hashes.
# Prints one JSON object: {"score": <avg FPS>, "info": {...}}.
set -u

cd "$(dirname "$0")/../.." || exit 1

GOLDEN=tools/native/bench_golden.json
SCRIPT=tools/wasm_replays/mudkip_starter.txt
BENCH=build/native/pokeemerald-bench

cleanup_isolated_build()
{
  case "$PWD" in
    */.optimize-anything/runs/*/worktree)
      chmod -R u+w build >/dev/null 2>&1 || true
      rm -rf build >/dev/null 2>&1 || true
      ;;
  esac
}

# The optimizer commits or resets immediately after this command exits.
# Remove ignored build outputs before handing control back so Git does
# not race thousands of generated asset files. Keep normal local runs
# incremental by cleaning only Optimize Anything worktrees.
trap cleanup_isolated_build 0

if [ ! -f "$GOLDEN" ] || [ ! -f "$SCRIPT" ]; then
  printf '{"score": 0, "info": {"error": "missing golden hashes or replay script"}}\n'
  exit 0
fi

NATIVE_CC=clang
if command -v ccache >/dev/null 2>&1 && command -v clang >/dev/null 2>&1; then
  NATIVE_CC="ccache clang"
fi

BUILD_LOG=$(mktemp /tmp/native_bench_build.XXXXXX.log)
# Use a serial build. Parallel asset prerequisites can outlive make's
# top-level process in isolated worktrees, racing the optimizer's cleanup
# and causing a valid improvement to be discarded.
if ! make NATIVE_CC="$NATIVE_CC" native-bench >"$BUILD_LOG" 2>&1; then
  python3 - "$BUILD_LOG" <<'PYEOF'
import json, sys
with open(sys.argv[1], errors="replace") as f:
    tail = f.read()[-2500:]
print(json.dumps({"score": 0, "info": {"error": "build failed", "build_log_tail": tail}}))
PYEOF
  rm -f "$BUILD_LOG"
  exit 0
fi
rm -f "$BUILD_LOG"

ERR_LOG=$(mktemp /tmp/native_bench_run.XXXXXX.log)
OUT=$("$BENCH" --script "$SCRIPT" --golden "$GOLDEN" --passes 2 2>"$ERR_LOG")
RC=$?
if [ $RC -ne 0 ] || [ -z "$OUT" ]; then
  python3 - "$RC" "$ERR_LOG" <<'PYEOF'
import json, sys
rc = sys.argv[1]
with open(sys.argv[2], errors="replace") as f:
    tail = f.read()[-2500:]
print(json.dumps({"score": 0, "info": {"error": "bench failed", "exit_code": rc, "stderr_tail": tail}}))
PYEOF
  rm -f "$ERR_LOG"
  exit 0
fi
rm -f "$ERR_LOG"
printf '%s\n' "$OUT"
