#!/usr/bin/env python3

from __future__ import annotations

import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MID2AGB = ROOT / "tools/mid2agb/mid2agb"
MIDI_DIR = ROOT / "sound/songs/midi"
SONG_TABLE = ROOT / "sound/song_table.inc"
MIDI_CFG = MIDI_DIR / "midi.cfg"

PLAYER_VALUES = {
    "MUSIC_PLAYER_BGM": 0,
    "MUSIC_PLAYER_SE1": 1,
    "MUSIC_PLAYER_SE2": 2,
    "MUSIC_PLAYER_SE3": 3,
}

WAIT_VALUES = {
    "00": 0,
    "01": 1,
    "02": 2,
    "03": 3,
    "04": 4,
    "05": 5,
    "06": 6,
    "07": 7,
    "08": 8,
    "09": 9,
    "10": 10,
    "11": 11,
    "12": 12,
    "13": 13,
    "14": 14,
    "15": 15,
    "16": 16,
    "17": 17,
    "18": 18,
    "19": 19,
    "20": 20,
    "21": 21,
    "22": 22,
    "23": 23,
    "24": 24,
    "28": 28,
    "30": 30,
    "32": 32,
    "36": 36,
    "40": 40,
    "42": 42,
    "44": 44,
    "48": 48,
    "52": 52,
    "54": 54,
    "56": 56,
    "60": 60,
    "64": 64,
    "66": 66,
    "68": 68,
    "72": 72,
    "76": 76,
    "78": 78,
    "80": 80,
    "84": 84,
    "88": 88,
    "90": 90,
    "92": 92,
    "96": 96,
}


def load_song_entries() -> list[tuple[str, int]]:
    entries = []
    song_re = re.compile(r"\bsong\s+(\w+),\s*(\w+),\s*\d+")

    for line in SONG_TABLE.read_text().splitlines():
        match = song_re.search(line)
        if match:
            entries.append((match.group(1), PLAYER_VALUES[match.group(2)]))

    return entries


def load_midi_options() -> dict[str, list[str]]:
    options = {}

    for raw in MIDI_CFG.read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line or ":" not in line:
            continue

        name, args = line.split(":", 1)
        stem = name.strip()
        if stem.endswith(".mid"):
            stem = stem[:-4]
        options[stem] = shlex.split(args)

    return options


def generate_song_asm(song_name: str, options: list[str], tmpdir: Path) -> Path | None:
    midi = MIDI_DIR / f"{song_name}.mid"
    if not midi.exists():
        return None

    asm_path = tmpdir / f"{song_name}.s"
    subprocess.run(
        [str(MID2AGB), str(midi), str(asm_path), *options],
        check=True,
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
    )
    return asm_path


def parse_track_count(lines: list[str], song_name: str) -> int:
    for index, line in enumerate(lines):
        if line.strip() == f"{song_name}:":
            for header_line in lines[index + 1:]:
                match = re.search(r"\.byte\s+(\d+)", header_line)
                if match:
                    return int(match.group(1))

    raise ValueError(f"missing song header for {song_name}")


def label_indices(lines: list[str]) -> dict[str, int]:
    labels = {}

    for index, line in enumerate(lines):
        match = re.match(r"^([A-Za-z_]\w*):\s*$", line.strip())
        if match:
            labels[match.group(1)] = index

    return labels


def word_target(lines: list[str], index: int) -> str:
    for line in lines[index + 1:]:
        match = re.search(r"\.word\s+([A-Za-z_]\w*)", line)
        if match:
            return match.group(1)
    raise ValueError("missing PATT target")


def label_duration(lines: list[str], labels: dict[str, int], label: str, stack: set[str]) -> int | None:
    if label in stack:
        return None

    stack.add(label)
    duration = 0
    index = labels[label] + 1

    while index < len(lines):
        line = lines[index]
        if re.match(r"^([A-Za-z_]\w*):\s*$", line.strip()):
            index += 1
            continue

        wait = re.search(r"\bW(\d\d)\b", line)
        if wait:
            duration += WAIT_VALUES[wait.group(1)]

        if re.search(r"\bGOTO\b", line):
            return None
        if re.search(r"\bFINE\b", line) or re.search(r"\bPEND\b", line):
            stack.remove(label)
            return duration
        if re.search(r"\bPATT\b", line):
            target = word_target(lines, index)
            pattern_duration = label_duration(lines, labels, target, stack)
            if pattern_duration is None:
                return None
            duration += pattern_duration

        index += 1

    stack.remove(label)
    return duration


def song_duration(asm_path: Path, song_name: str) -> int:
    lines = asm_path.read_text().splitlines()
    track_count = parse_track_count(lines, song_name)
    labels = label_indices(lines)
    durations = []

    for track in range(1, track_count + 1):
        duration = label_duration(lines, labels, f"{song_name}_{track}", set())
        if duration is None:
            return 0
        durations.append(duration)

    return max(1, max(durations, default=0))


def song_durations(entries: list[tuple[str, int]], options: dict[str, list[str]]) -> list[int]:
    durations = []

    with tempfile.TemporaryDirectory(prefix="pokeemerald-wasm-sound-") as tmp:
        tmpdir = Path(tmp)
        cache: dict[str, int] = {}

        for song_name, _player in entries:
            if song_name not in cache:
                asm_path = generate_song_asm(song_name, options.get(song_name, []), tmpdir)
                cache[song_name] = 0 if asm_path is None else song_duration(asm_path, song_name)
            durations.append(cache[song_name])

    return durations


def write_header(path: Path, entries: list[tuple[str, int]], durations: list[int]) -> None:
    lines = [
        "// Generated by tools/generate_wasm_sound.py. Do not edit.",
        "#ifndef GUARD_WASM_SOUND_H",
        "#define GUARD_WASM_SOUND_H",
        "",
        "#define WASM_MUSIC_PLAYER_BGM 0",
        "#define WASM_MUSIC_PLAYER_SE1 1",
        "#define WASM_MUSIC_PLAYER_SE2 2",
        "#define WASM_MUSIC_PLAYER_SE3 3",
        f"#define WASM_SONG_COUNT {len(entries)}",
        "",
        "struct WasmSongInfo",
        "{",
        "    u8 player;",
        "    u16 duration;",
        "};",
        "",
        "static const struct WasmSongInfo gWasmSongInfo[WASM_SONG_COUNT] =",
        "{",
    ]

    for index, ((song_name, player), duration) in enumerate(zip(entries, durations)):
        lines.append(f"    [{index}] = {{{player}, {duration}}}, // {song_name}")

    lines.extend([
        "};",
        "",
        "#endif // GUARD_WASM_SOUND_H",
        "",
    ])

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines))


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: generate_wasm_sound.py OUTPUT")

    entries = load_song_entries()
    durations = song_durations(entries, load_midi_options())
    write_header(Path(sys.argv[1]), entries, durations)


if __name__ == "__main__":
    main()
