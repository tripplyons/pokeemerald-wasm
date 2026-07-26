"""Optimize the complete WASM/native boundary with the fixed FPS benchmark."""

from __future__ import annotations

import json
import math
from pathlib import Path
import shutil
import subprocess
import time

from code_workspace import evaluate_code_candidate, load_code_candidate, run_workspace_command


PROJECT_ROOT = Path(__file__).resolve().parents[2]
CODE_PATHS = (
    "Makefile",
    "tools/native/native_engine.c",
    "tools/native/native_engine.h",
    "tools/native/performance.mk",
    "src/main.c",
    "src/m4a.c",
    "include/wasm/stdio.h",
    "include/wasm/stdlib.h",
    "include/wasm/string.h",
    "src/wasm_display.c",
    "src/wasm_field_effect_scripts.c",
    "tools/wasm_asm_data.py",
)
BENCHMARK_COMMAND = ("bash", "tools/native/run_bench.sh")
BENCHMARK_TIMEOUT_SECONDS = 1200
BUILD_CACHE_DIRS = ("build/assets", "build/wasm")
NATIVE_CACHE_FILES = (
    "build/native/pokeemerald_wasm2c.c",
    "build/native/pokeemerald_wasm2c.h",
    "build/native/pokeemerald_wasm2c.o",
)
WASM_OBJECTS = {
    "src/main.c": "build/wasm/obj/main.o",
    "src/m4a.c": "build/wasm/obj/m4a.o",
    "src/wasm_display.c": "build/wasm/obj/wasm_display.o",
    "src/wasm_field_effect_scripts.c": "build/wasm/obj/wasm_field_effect_scripts.o",
}
FULL_WASM_REBUILD_PATHS = {
    "Makefile",
    "tools/native/performance.mk",
    "include/wasm/stdio.h",
    "include/wasm/stdlib.h",
    "include/wasm/string.h",
    "tools/wasm_asm_data.py",
}
NATIVE_OUTPUTS = (
    "build/native/pokeemerald_wasm2c.c",
    "build/native/pokeemerald_wasm2c.h",
    "build/native/pokeemerald_wasm2c.o",
    "build/native/native_engine.o",
    "build/native/bench_main.o",
    "build/native/pokeemerald-bench",
)


def find_wasm_ld() -> str | None:
    executable = shutil.which("wasm-ld")
    if executable:
        return executable
    toolchains = Path.home() / ".rustup/toolchains"
    return next((str(path) for path in toolchains.glob("*/lib/rustlib/*/bin/gcc-ld/wasm-ld")), None)


WASM_LD = find_wasm_ld()


def changed_paths(workspace: Path) -> list[str]:
    changed = []
    for relative in CODE_PATHS:
        candidate = workspace / relative
        seed = PROJECT_ROOT / relative
        if not candidate.is_file() or candidate.read_bytes() != seed.read_bytes():
            changed.append(relative)
    return changed


def make_block(lines: list[str], prefix: str) -> tuple[str, ...]:
    for index, line in enumerate(lines):
        if not line.startswith(prefix):
            continue
        block = [line]
        for following in lines[index + 1 :]:
            if following.startswith(("\t", " ")) or not following:
                block.append(following)
                continue
            break
        return tuple(block)
    return ()


def benchmark_wiring_changed(workspace: Path) -> bool:
    candidate = (workspace / "Makefile").read_text().splitlines()
    seed = (PROJECT_ROOT / "Makefile").read_text().splitlines()
    protected_prefixes = (
        "NATIVE_BENCH_O :=",
        "NATIVE_BENCH :=",
        "native-bench:",
        "$(NATIVE_BENCH_O):",
        "$(NATIVE_BENCH):",
    )
    return any(make_block(candidate, prefix) != make_block(seed, prefix) for prefix in protected_prefixes)


def clone_path(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    clone_flag = "-cR" if source.is_dir() else "-c"
    completed = subprocess.run(
        ("cp", clone_flag, str(source), str(destination)),
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode == 0:
        return
    if source.is_dir():
        shutil.copytree(source, destination)
    else:
        shutil.copy2(source, destination)


def clone_build_cache(workspace: Path) -> None:
    for relative in (*BUILD_CACHE_DIRS, *NATIVE_CACHE_FILES):
        source = PROJECT_ROOT / relative
        if source.exists():
            clone_path(source, workspace / relative)

    # Worktree files are freshly checked out. Make copied outputs newer before
    # deleting the outputs that actually depend on this candidate's changes.
    for relative in ("build/assets", "build/wasm", "build/native"):
        root = workspace / relative
        if not root.exists():
            continue
        for output in root.rglob("*"):
            if output.is_file():
                output.touch()


def remove_path(path: Path) -> None:
    if path.is_dir():
        shutil.rmtree(path)
    else:
        path.unlink(missing_ok=True)


def invalidate_candidate_outputs(workspace: Path, changed: list[str]) -> str:
    if any(relative in FULL_WASM_REBUILD_PATHS for relative in changed):
        remove_path(workspace / "build/wasm/obj")
        mode = "full-wasm"
    else:
        rebuilt_objects = []
        for relative, output in WASM_OBJECTS.items():
            if relative in changed:
                remove_path(workspace / output)
                rebuilt_objects.append(output)
        mode = "targeted-wasm" if rebuilt_objects else "native-only"

    wasm_changed = mode != "native-only"
    if wasm_changed:
        remove_path(workspace / "build/wasm/pokeemerald.wasm")
        for relative in NATIVE_OUTPUTS:
            remove_path(workspace / relative)
    else:
        if "tools/native/native_engine.c" in changed:
            remove_path(workspace / "build/native/native_engine.o")
            remove_path(workspace / "build/native/pokeemerald-bench")
        if "tools/native/native_engine.h" in changed:
            remove_path(workspace / "build/native/native_engine.o")
            remove_path(workspace / "build/native/bench_main.o")
            remove_path(workspace / "build/native/pokeemerald-bench")
    return mode


def run_benchmark(workspace: Path) -> tuple[float, dict]:
    changed = changed_paths(workspace)
    if "Makefile" in changed and benchmark_wiring_changed(workspace):
        return 0.0, {
            "error": "candidate changed protected native benchmark build wiring",
            "changed_files": changed,
        }

    invalidation = "primary-checkout"
    if workspace.resolve() != PROJECT_ROOT.resolve():
        clone_build_cache(workspace)
        invalidation = invalidate_candidate_outputs(workspace, changed)

    started = time.monotonic()
    try:
        completed = run_workspace_command(
            workspace,
            BENCHMARK_COMMAND,
            timeout=BENCHMARK_TIMEOUT_SECONDS,
            env={**({"WASM_LD": WASM_LD} if WASM_LD else {})},
        )
    except subprocess.TimeoutExpired as timeout:
        return 0.0, {
            "error": f"benchmark timed out after {timeout.timeout} seconds",
            "changed_files": changed,
            "cache_invalidation": invalidation,
        }
    except OSError as error:
        return 0.0, {
            "error": str(error),
            "changed_files": changed,
            "cache_invalidation": invalidation,
        }

    elapsed = round(time.monotonic() - started, 3)
    if completed.returncode != 0:
        return 0.0, {
            "error": f"benchmark exited {completed.returncode}",
            "changed_files": changed,
            "cache_invalidation": invalidation,
            "evaluator_seconds": elapsed,
            "stdout_tail": completed.stdout[-4000:],
            "stderr_tail": completed.stderr[-4000:],
        }
    try:
        result = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        return 0.0, {
            "error": f"benchmark output is not JSON: {error}",
            "changed_files": changed,
            "cache_invalidation": invalidation,
            "evaluator_seconds": elapsed,
            "stdout_tail": completed.stdout[-4000:],
            "stderr_tail": completed.stderr[-4000:],
        }

    score = result.get("score") if isinstance(result, dict) else None
    info = result.get("info") if isinstance(result, dict) else None
    if isinstance(score, bool) or not isinstance(score, (int, float)) or not math.isfinite(score):
        return 0.0, {"error": "benchmark score is not finite and numeric", "output": result}
    if not isinstance(info, dict):
        return 0.0, {"error": "benchmark info is not an object", "output": result}

    info["changed_files"] = changed
    info["cache_invalidation"] = invalidation
    info["evaluator_seconds"] = elapsed
    scenario_fps = info.get("scenario_fps")
    if isinstance(scenario_fps, dict) and scenario_fps:
        info["slowest_scenario"] = min(scenario_fps, key=scenario_fps.get)
    binary = workspace / "build/native/pokeemerald-bench"
    if binary.is_file():
        info["binary_bytes"] = binary.stat().st_size
    return float(score), info


def evaluate_workspace(workspace: Path, _example=None) -> tuple[float, dict]:
    return run_benchmark(workspace)


def evaluate(candidate: object) -> tuple[float, dict]:
    return evaluate_code_candidate(PROJECT_ROOT, CODE_PATHS, candidate, evaluate_workspace)


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
        "objective": "Maximize deterministic headless native pokeemerald engine throughput, scored as the arithmetic mean aggregate WasmRunFrame FPS across fixed overworld, menu, and battle scenarios, while preserving all golden framebuffer hashes, deterministic progression, native frontend behavior, browser WASM behavior, Kindle behavior, and the normal non-WASM GBA build.",
        "background": "The candidate contains the complete approved WASM/native boundary files. The fixed benchmark, runner, replay, golden hashes, and evaluator are immutable. Candidate worktrees receive only immutable cached build prerequisites; outputs affected by candidate edits are deleted and rebuilt. Two benchmark passes each replay blank-save overworld, menu, and battle states, warm 10,007 frames, time exactly 1,000,003 WasmRunFrame calls per scenario with rendering outside timing, and check six framebuffer hashes plus determinism and progression. Score 0 for build/runtime failures, hash mismatches, benchmark wiring changes, skipped work, hardcoded scenarios or hashes, undefined behavior, or regressions to browser WASM, Kindle, native-raylib, or normal GBA behavior. Shared source optimizations must be WASM-guarded when behavior differs. Display-only edits cannot legitimately improve this score. Prefer small general optimizations supported by per-scenario ASI.",
    }
