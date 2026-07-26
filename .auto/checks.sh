#!/bin/bash
# Correctness backpressure: reject oracle tampering, re-verify equivalence.
set -euo pipefail

OFF_LIMITS=(
  tools/native/bench_main.c
  tools/native/run_bench.sh
  tools/native/bench_golden.json
  tools/wasm_replays/mudkip_starter.txt
  tools/wasm_replays/hblank_dma_win0h_probe.txt
)

changed="$( { git diff --name-only HEAD; git ls-files --others --exclude-standard; } 2>/dev/null | grep -v '^\.auto/' | sort -u || true)"
for f in "${OFF_LIMITS[@]}"; do
  if printf '%s\n' "$changed" | grep -qx "$f"; then
    echo "CHECKS FAILED: candidate modified immutable oracle file: $f" >&2
    exit 1
  fi
done

OUT="$(./build/native/pokeemerald-bench --script tools/wasm_replays/mudkip_starter.txt --golden tools/native/bench_golden.json --passes 2 2>/dev/null)" || OUT=""
python3 - "$OUT" <<'PYEOF'
import json, sys
try:
    d = json.loads(sys.argv[1])
except Exception:
    sys.stderr.write("checks: bench produced no/invalid output\n")
    sys.exit(1)
info = d.get("info", {})
if not (d.get("score", 0) > 0 and info.get("hashes_ok") and info.get("deterministic") and info.get("progressing") and not info.get("mismatches")):
    sys.stderr.write(f"checks: equivalence gate failed: {info.get('mismatches')} hashes_ok={info.get('hashes_ok')} det={info.get('deterministic')}\n")
    sys.exit(1)
PYEOF
