#!/bin/bash
# Autoresearch benchmark for pokeemerald native engine FPS.
# Runs with cwd = the candidate worktree. Builds the headless bench target,
# replays the fixed mudkip_starter scenarios, and emits METRIC lines.
#
# Correctness is mandatory: any golden-hash mismatch, nondeterminism, or
# non-progressing run is a hard failure (exit 1), never a zero-score "keep".
# The benchmark/oracle files are immutable; optimizing them is cheating.
set -euo pipefail

MAIN_REPO="$(cd "$(dirname "$0")/.." && pwd)"
GOLDEN=tools/native/bench_golden.json
SCRIPT=tools/wasm_replays/mudkip_starter.txt
BENCH=build/native/pokeemerald-bench

if [ ! -f "$GOLDEN" ] || [ ! -f "$SCRIPT" ]; then
  echo "METRIC fps=0"
  echo "missing golden hashes or replay script" >&2
  exit 1
fi

NATIVE_CC=clang
if command -v ccache >/dev/null 2>&1 && command -v clang >/dev/null 2>&1; then
  NATIVE_CC="ccache clang"
fi

# --- Build cache: reuse the main checkout's generated assets/wasm/native -----
if [ "$(pwd)" != "$MAIN_REPO" ]; then
  for d in assets wasm native; do
    if [ -d "$MAIN_REPO/build/$d" ] && [ ! -d "build/$d" ]; then
      mkdir -p build
      cp -R "$MAIN_REPO/build/$d" "build/$d"
    fi
  done
  if [ -d build ]; then
    find build -type f -exec touch {} + 2>/dev/null || true
  fi

  changed="$(git diff --name-only HEAD 2>/dev/null || true)"
  changed_untracked="$(git ls-files --others --exclude-standard 2>/dev/null | grep -v '^\.auto/' || true)"
  all_changed="$(printf '%s\n%s\n' "$changed" "$changed_untracked" | grep -v '^$' | sort -u || true)"

  only_native=1
  only_src=1
  while IFS= read -r f; do
    [ -z "$f" ] && continue
    case "$f" in
      tools/native/performance.mk) only_native=0; only_src=0 ;;
      tools/native/*) ;;
      src/*.c) only_native=0 ;;
      *) only_native=0; only_src=0 ;;
    esac
  done <<< "$all_changed"

  if [ -z "$all_changed" ]; then
    :
  elif [ "$only_native" = 1 ]; then
    rm -f build/native/native_engine.o build/native/raylib_main.o \
          build/native/bench_main.o build/native/pokeemerald-bench \
          build/native/pokeemerald-native 2>/dev/null || true
  elif [ "$only_src" = 1 ]; then
    while IFS= read -r f; do
      case "$f" in
        src/*.c)
          base="$(basename "$f" .c)"
          rm -f "build/wasm/obj/$base.o" 2>/dev/null || true
          ;;
      esac
    done <<< "$all_changed"
    rm -f build/wasm/pokeemerald.wasm 2>/dev/null || true
    rm -rf build/native 2>/dev/null || true
  else
    rm -rf build/wasm build/native 2>/dev/null || true
  fi
fi

# --- Build -------------------------------------------------------------------
BUILD_LOG="$(mktemp /tmp/autoresearch_build.XXXXXX)"
if ! make NATIVE_CC="$NATIVE_CC" native-bench >"$BUILD_LOG" 2>&1; then
  echo "METRIC fps=0"
  echo "build failed:" >&2
  tail -25 "$BUILD_LOG" >&2
  rm -f "$BUILD_LOG"
  exit 1
fi
rm -f "$BUILD_LOG"

# --- Serialize the timed region across concurrent arms -----------------------
LOCK="$MAIN_REPO/.auto/.benchlock"
for _ in $(seq 1 600); do
  if mkdir "$LOCK" 2>/dev/null; then break; fi
  sleep 0.5
done
trap 'rmdir "$LOCK" 2>/dev/null || true' EXIT

OUT="$("$BENCH" --script "$SCRIPT" --golden "$GOLDEN" --passes 3 2>/dev/null)" || OUT=""
rmdir "$LOCK" 2>/dev/null || true
trap - EXIT

if [ -z "$OUT" ]; then
  echo "METRIC fps=0"
  echo "bench produced no output" >&2
  exit 1
fi

python3 - "$OUT" <<'PYEOF'
import json, sys
try:
    d = json.loads(sys.argv[1])
except Exception as e:
    print("METRIC fps=0")
    sys.stderr.write(f"bench output not JSON: {e}\n")
    sys.exit(1)
info = d.get("info", {})
score = d.get("score", 0)
ok = (
    isinstance(score, (int, float)) and score > 0
    and info.get("hashes_ok") is True
    and info.get("deterministic") is True
    and info.get("progressing") is True
    and info.get("sprite_sort_check") is True
    and not info.get("mismatches")
)
if not ok:
    print("METRIC fps=0")
    sys.stderr.write(
        "correctness gate failed: "
        f"score={score} hashes_ok={info.get('hashes_ok')} "
        f"det={info.get('deterministic')} prog={info.get('progressing')} "
        f"sprite_sort={info.get('sprite_sort_check')} "
        f"mismatches={info.get('mismatches')}\n"
    )
    sys.exit(1)
scen = info.get("scenario_fps", {})
print(f"METRIC fps={score:.2f}")
print(f"METRIC overworld_fps={scen.get('overworld', 0):.2f}")
print(f"METRIC menu_fps={scen.get('menu', 0):.2f}")
print(f"METRIC battle_fps={scen.get('battle', 0):.2f}")
PYEOF
