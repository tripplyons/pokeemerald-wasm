"""Evaluate native compiler flags with the deterministic headless benchmark."""

from __future__ import annotations

import json
import math
import os
from pathlib import Path
import re
import shlex
import subprocess
import time


PROJECT_ROOT = Path(__file__).resolve().parents[2]
SEED_CANDIDATE_PATH = PROJECT_ROOT / ".optimize-anything/candidates/native-engine-fps-3.txt"
BENCHMARK_COMMAND = ("bash", "tools/native/run_bench.sh")
BENCHMARK_TIMEOUT_SECONDS = 1200
EXPECTED_KEYS = {"NATIVE_CFLAGS", "NATIVE_LDFLAGS"}
SAFE_FLAG = re.compile(r"-[A-Za-z0-9][A-Za-z0-9_+.,=:/-]*")
BLOCKED_PREFIXES = (
    "-Ofast",
    "-ffast-math",
    "-funsafe-math-optimizations",
    "-ffinite-math-only",
    "-fno-signed-zeros",
    "-fassociative-math",
    "-freciprocal-math",
    "-fprofile",
    "-fplugin",
    "-save-temps",
    "-ftime-trace",
)
BLOCKED_EXACT = {
    "-c",
    "-E",
    "-S",
    "-o",
    "-include",
    "-imacros",
    "-isysroot",
    "-Xclang",
}


def invalid_candidate(error: str, candidate: object) -> tuple[float, dict]:
    return 0.0, {"error": error, "candidate": str(candidate)[:2000]}


def parse_flags(name: str, value: object) -> tuple[str | None, str | None]:
    if not isinstance(value, str):
        return None, f"{name} must be a string"
    if len(value) > 1000:
        return None, f"{name} exceeds 1000 characters"
    try:
        tokens = shlex.split(value)
    except ValueError as error:
        return None, f"{name} cannot be tokenized: {error}"
    if len(tokens) > 32:
        return None, f"{name} exceeds 32 flags"
    for token in tokens:
        if not SAFE_FLAG.fullmatch(token):
            return None, f"{name} contains unsafe or non-flag token: {token!r}"
        if token in BLOCKED_EXACT or any(token.startswith(prefix) for prefix in BLOCKED_PREFIXES):
            return None, f"{name} contains blocked flag: {token}"
        if token.startswith(("-D", "-U")) and token != "-DNDEBUG":
            return None, f"{name} may define only the NDEBUG preprocessor symbol"
    return " ".join(tokens), None


def parse_candidate(candidate: object) -> tuple[dict[str, str] | None, str | None]:
    if not isinstance(candidate, str):
        return None, "candidate must be JSON text"
    try:
        payload = json.loads(candidate)
    except json.JSONDecodeError as error:
        return None, f"candidate is not valid JSON: {error}"
    if not isinstance(payload, dict):
        return None, "candidate must be a JSON object"
    if set(payload) != EXPECTED_KEYS:
        return None, f"candidate keys must be exactly {sorted(EXPECTED_KEYS)}"

    normalized = {}
    for name in sorted(EXPECTED_KEYS):
        value, error = parse_flags(name, payload[name])
        if error:
            return None, error
        normalized[name] = value
    return normalized, None


def run_command(command: tuple[str, ...], env: dict[str, str], timeout: int) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=PROJECT_ROOT,
        env=env,
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )


def evaluate(candidate: str) -> tuple[float, dict]:
    """Return benchmark FPS and diagnostics for one compiler-flag candidate."""
    flags, error = parse_candidate(candidate)
    if error:
        return invalid_candidate(error, candidate)

    env = os.environ.copy()
    env.update(flags)
    started = time.monotonic()
    try:
        clean = run_command(("make", "clean-native"), env, timeout=120)
        if clean.returncode != 0:
            return 0.0, {
                "error": "make clean-native failed",
                "flags": flags,
                "stderr_tail": clean.stderr[-4000:],
            }
        completed = run_command(BENCHMARK_COMMAND, env, timeout=BENCHMARK_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired as timeout:
        return 0.0, {
            "error": f"benchmark timed out after {timeout.timeout} seconds",
            "flags": flags,
        }
    except OSError as error:
        return 0.0, {"error": str(error), "flags": flags}

    if completed.returncode != 0:
        return 0.0, {
            "error": f"benchmark exited {completed.returncode}",
            "flags": flags,
            "stdout_tail": completed.stdout[-4000:],
            "stderr_tail": completed.stderr[-4000:],
        }
    try:
        result = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        return 0.0, {
            "error": f"benchmark output is not JSON: {error}",
            "flags": flags,
            "stdout_tail": completed.stdout[-4000:],
            "stderr_tail": completed.stderr[-4000:],
        }

    score = result.get("score") if isinstance(result, dict) else None
    info = result.get("info") if isinstance(result, dict) else None
    if isinstance(score, bool) or not isinstance(score, (int, float)) or not math.isfinite(score):
        return 0.0, {"error": "benchmark score is not finite and numeric", "flags": flags, "output": result}
    if not isinstance(info, dict):
        return 0.0, {"error": "benchmark info is not an object", "flags": flags, "output": result}

    info["flags"] = flags
    info["evaluator_seconds"] = round(time.monotonic() - started, 3)
    scenario_fps = info.get("scenario_fps")
    if isinstance(scenario_fps, dict) and scenario_fps:
        info["slowest_scenario"] = min(scenario_fps, key=scenario_fps.get)
    return float(score), info


def preflight() -> dict:
    candidate = SEED_CANDIDATE_PATH.read_text()
    score, info = evaluate(candidate)
    return {"baseline_score": score, "info": info}


def build_task() -> dict:
    task = {
        "seed_candidate": SEED_CANDIDATE_PATH.read_text(),
        "evaluator": evaluate,
        "preflight": preflight,
        "objective": "Maximize deterministic headless native pokeemerald engine throughput, measured as the arithmetic mean of aggregate WasmRunFrame FPS across the fixed overworld, menu, and battle scenarios, while preserving every golden framebuffer hash and all benchmark correctness checks.",
        "background": "The optimized text artifact is a strict JSON object containing exactly NATIVE_CFLAGS and NATIVE_LDFLAGS string values for the Darwin arm64 desktop wasm2c build. The evaluator must validate and safely tokenize these flags, reject make/shell metacharacters and non-flag arguments, clean build/native before every measurement, inject the two values through the environment, and run bash tools/native/run_bench.sh. WASM_OPT_FLAGS remains fixed at the proven -O2. The benchmark opens no GUI, replays tools/wasm_replays/mudkip_starter.txt from a blank save, warms each scenario for 10,007 frames, times exactly 1,000,003 WasmRunFrame calls per scenario over two passes, and checks six committed start/end framebuffer hashes. Score 0 for malformed candidates, build/runtime failures, hash mismatches, nondeterminism, or invalid timing. Return rich info including per-pass and per-scenario FPS, accepted normalized flags, errors, and build diagnostics. Current proven seed uses Clang -O3 -DNDEBUG -fomit-frame-pointer -flto and linker -flto and scores about 2.0 million aggregate FPS on this M3 Max. Do not edit benchmark files, golden hashes, source, replay inputs, normal GBA settings, Kindle settings, or WASM flags. Avoid fast-math and undefined-behavior flags; correctness hashes are mandatory but not a license to weaken semantics.",
    }
    return task
