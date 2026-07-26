"""Optimize the native host engine source with the fixed FPS benchmark."""

from __future__ import annotations

import json
import math
from pathlib import Path
import shutil
import subprocess
import time

from code_workspace import evaluate_code_candidate, load_code_candidate, run_workspace_command


PROJECT_ROOT = Path(__file__).resolve().parents[2]
CODE_PATHS = ("tools/native/native_engine.c",)
BENCHMARK_COMMAND = ("bash", "tools/native/run_bench.sh")
BENCHMARK_TIMEOUT_SECONDS = 1200
BUILD_CACHE_PATHS = ("build/assets", "build/wasm")
NATIVE_CACHE_PATHS = (
    "build/native/pokeemerald_wasm2c.c",
    "build/native/pokeemerald_wasm2c.h",
    "build/native/pokeemerald_wasm2c.o",
)
FROZEN_MAKE_TARGETS = ("build/wasm/pokeemerald.wasm", *NATIVE_CACHE_PATHS)


def find_wasm_ld() -> str | None:
    executable = shutil.which("wasm-ld")
    if executable:
        return executable
    toolchains = Path.home() / ".rustup/toolchains"
    return next((str(path) for path in toolchains.glob("*/lib/rustlib/*/bin/gcc-ld/wasm-ld")), None)


WASM_LD = find_wasm_ld()


def clone_build_cache(workspace: Path) -> None:
    """Clone immutable build prerequisites so each worktree need only rebuild native code."""
    for relative in BUILD_CACHE_PATHS:
        source = PROJECT_ROOT / relative
        destination = workspace / relative
        if not source.is_dir():
            continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        completed = subprocess.run(
            ("cp", "-cR", str(source), str(destination)),
            capture_output=True,
            text=True,
            check=False,
        )
        if completed.returncode != 0:
            shutil.copytree(source, destination)
    for relative in NATIVE_CACHE_PATHS:
        source = PROJECT_ROOT / relative
        destination = workspace / relative
        if not source.is_file():
            continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        completed = subprocess.run(
            ("cp", "-c", str(source), str(destination)),
            capture_output=True,
            text=True,
            check=False,
        )
        if completed.returncode != 0:
            shutil.copy2(source, destination)


def run_benchmark(workspace: Path) -> tuple[float, dict]:
    if workspace.resolve() != PROJECT_ROOT.resolve():
        clone_build_cache(workspace)
    started = time.monotonic()
    try:
        completed = run_workspace_command(
            workspace,
            BENCHMARK_COMMAND,
            timeout=BENCHMARK_TIMEOUT_SECONDS,
            env={
                "NATIVE_MAKE_OLD_FILES": " ".join(FROZEN_MAKE_TARGETS),
                **({"WASM_LD": WASM_LD} if WASM_LD else {}),
            },
        )
    except subprocess.TimeoutExpired as timeout:
        return 0.0, {"error": f"benchmark timed out after {timeout.timeout} seconds"}
    except OSError as error:
        return 0.0, {"error": str(error)}

    if completed.returncode != 0:
        return 0.0, {
            "error": f"benchmark exited {completed.returncode}",
            "stdout_tail": completed.stdout[-4000:],
            "stderr_tail": completed.stderr[-4000:],
        }
    try:
        result = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        return 0.0, {
            "error": f"benchmark output is not JSON: {error}",
            "stdout_tail": completed.stdout[-4000:],
            "stderr_tail": completed.stderr[-4000:],
        }

    score = result.get("score") if isinstance(result, dict) else None
    info = result.get("info") if isinstance(result, dict) else None
    if isinstance(score, bool) or not isinstance(score, (int, float)) or not math.isfinite(score):
        return 0.0, {"error": "benchmark score is not finite and numeric", "output": result}
    if not isinstance(info, dict):
        return 0.0, {"error": "benchmark info is not an object", "output": result}

    info["candidate_files"] = list(CODE_PATHS)
    info["evaluator_seconds"] = round(time.monotonic() - started, 3)
    scenario_fps = info.get("scenario_fps")
    if isinstance(scenario_fps, dict) and scenario_fps:
        info["slowest_scenario"] = min(scenario_fps, key=scenario_fps.get)
    return float(score), info


def evaluate_workspace(workspace: Path, _example=None) -> tuple[float, dict]:
    return run_benchmark(workspace)


def evaluate(candidate: object) -> tuple[float, dict]:
    return evaluate_code_candidate(
        PROJECT_ROOT,
        CODE_PATHS,
        candidate,
        evaluate_workspace,
    )


def preflight() -> dict:
    seed = load_code_candidate(PROJECT_ROOT, CODE_PATHS)
    score, info = evaluate({**seed, "refiner_prompt": "preflight metadata-shape check"})
    if score <= 0 or info.get("error"):
        raise RuntimeError(f"baseline benchmark failed: {info.get('error', 'non-positive score')}")
    return {"baseline_score": score, "info": info}


def build_task() -> dict:
    return {
        "seed_candidate": load_code_candidate(PROJECT_ROOT, CODE_PATHS),
        "code_paths": CODE_PATHS,
        "evaluator": evaluate,
        "preflight": preflight,
        "objective": "Maximize deterministic headless native pokeemerald engine throughput, measured as the arithmetic mean of aggregate WasmRunFrame FPS across the fixed overworld, menu, and battle scenarios, while preserving every golden framebuffer hash and all benchmark correctness checks.",
        "background": "The candidate is a component dict containing the complete tools/native/native_engine.c source. Native GEPA directly edits that host-only implementation; every candidate runs in a detached Git worktree. The benchmark, golden hashes, replay, generated wasm2c module, compiler flags, WASM flags, Makefile, frontend, evaluator, and all game source are immutable. The benchmark opens no GUI, replays tools/wasm_replays/mudkip_starter.txt from a blank save, warms each scenario for 10,007 frames, times exactly 1,000,003 WasmRunFrame calls per scenario over two passes, and checks six start/end framebuffer hashes. Score 0 for build/runtime failures, hash mismatches, nondeterminism, or invalid timing. Preserve C correctness, host syscall semantics, native-raylib compatibility, normal GBA behavior, and Kindle behavior. Favor small semantics-preserving changes to hot host shims; do not hardcode scenarios, skip work, predict hashes, weaken checks, add undefined behavior, or attempt to edit files outside the candidate component.",
    }
