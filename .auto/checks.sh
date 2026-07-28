#!/bin/bash
# Correctness backpressure: reject oracle tampering, re-verify equivalence.
set -euo pipefail

MAIN_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
BASE_COMMIT="$(git -C "$MAIN_REPO" rev-parse HEAD)"
IMMUTABLE_PATHS=(
  .auto
  tools/native/bench_main.c
  tools/native/run_bench.sh
  tools/native/bench_golden.json
  tools/wasm_replays
)

# Compare the candidate against the main checkout's HEAD. Candidate commits
# therefore cannot hide oracle changes from the check. Include untracked files
# even when a candidate adds an ignore rule for them.
changed="$({
  git diff --name-only "$BASE_COMMIT" -- "${IMMUTABLE_PATHS[@]}"
  git ls-files --others -- "${IMMUTABLE_PATHS[@]}"
} | LC_ALL=C sort -u |
  grep -Ev '^\.auto/(log\.jsonl|dashboard\.json|[^/]*\.tmp|\.benchlock(/.*)?)$' || true)"

if [[ -n "$changed" ]]; then
  echo "CHECKS FAILED: candidate modified immutable benchmark/oracle files:" >&2
  printf '%s\n' "$changed" >&2
  exit 1
fi

BUILD_LOG="$(mktemp /tmp/autoresearch_checks_build.XXXXXX)"
trap 'rm -f "$BUILD_LOG"' EXIT
if ! make NATIVE_CC=clang native-bench >"$BUILD_LOG" 2>&1; then
  echo "checks: candidate benchmark build failed:" >&2
  tail -25 "$BUILD_LOG" >&2
  exit 1
fi
rm -f "$BUILD_LOG"
trap - EXIT

OUT="$(./build/native/pokeemerald-bench --script tools/wasm_replays/mudkip_starter.txt --golden tools/native/bench_golden.json --passes 2 2>/dev/null)" || OUT=""
python3 - "$OUT" <<'PYEOF'
import json, sys
try:
    d = json.loads(sys.argv[1])
except Exception:
    sys.stderr.write("checks: bench produced no/invalid output\n")
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
    sys.stderr.write(
        "checks: equivalence gate failed: "
        f"score={score} hashes_ok={info.get('hashes_ok')} "
        f"det={info.get('deterministic')} prog={info.get('progressing')} "
        f"sprite_sort={info.get('sprite_sort_check')} "
        f"mismatches={info.get('mismatches')}\n"
    )
    sys.exit(1)
PYEOF
