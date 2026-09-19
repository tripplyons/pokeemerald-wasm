# Native performance measurements

Measured on an Apple M3 Max, macOS 26.6.2, with Apple clang 21.0.0 and the default
`-O3 -flto` native flags. No PGO profile was present. These are headless
simulation results, not displayed game FPS.

## OAM matrix writes

The baseline is commit `73e2fc38c`. Sampling its eight-pass benchmark at 1 ms
intervals found 742 top-of-stack samples in `BuildOamBuffer`, 469 in
`UpdateOamCoords`, and 476 in memory copies, out of 4,336 total samples.
The sample includes replay, rendering, and startup as well as timed simulation.
Pointer encoding accounted for only eight samples, so it was not changed.

The native OAM helper filled unused records, then separately wrote all 128
matrix components. It now combines the fill and matrix component for unused
records, while preserving the attributes of active records and records beyond
the OAM limit. This removes duplicate writes without caching game state.
A first version selected active/unused records inside one loop and regressed;
the retained version separates those ranges.

Results below are medians of three runs per binary, alternating run order.
Each run used eight passes, the committed Mudkip replay, 10,007 warmup frames,
and 1,000,003 measured frames per scenario. The binaries were built before
measurement and run sequentially without sampling or compilation alongside.

| Scenario | Baseline frames/s | OAM frames/s | Change |
| --- | ---: | ---: | ---: |
| Overworld | 4,593,734 | 4,756,424 | +3.5% |
| Menu | 11,022,475 | 11,723,588 | +6.4% |
| Battle | 5,766,232 | 5,974,416 | +3.6% |
| Aggregate score | 7,129,347 | 7,484,339 | +5.0% |

Rendering remained approximately 97/87/89 microseconds per frame for
overworld/menu/battle. It is measured separately and excluded from the score.
All runs matched the six unchanged framebuffer goldens and passed the sprite
sort and determinism checks. `make native-test` also passed, including all
16,641 OAM count/limit combinations with unaligned buffers and guard bytes.

Run the existing benchmark with:

```sh
make native-bench native-test
build/native/pokeemerald-bench --script tools/wasm_replays/mudkip_starter.txt --golden tools/native/bench_golden.json --passes 8
```

Local raw results and the sampling report are in the ignored
`build/native/perf/` directory. Results do not establish Kindle, browser,
GUI, or PGO performance.

## Follow-up checks and remaining bottlenecks

A second round of three alternating eight-pass runs reproduced the OAM gain:
median aggregate scores were 7,037,959 before and 7,412,842 after (+5.3%).
A zero-rotation `ObjAffineSet` shortcut passed exhaustive angle/scale checks
but scored 7,408,476 in the same round, effectively unchanged from OAM alone.
It was removed. Both OAM and the experimental affine path also passed a
standalone AddressSanitizer/UndefinedBehaviorSanitizer run of the BIOS checks.

Remaining candidates from the baseline profile, in priority order:

- `UpdateOamCoords`: 469 samples. Position and sort-key calculations run for
  active sprites every frame. Any caching must account for direct writes to
  sprite fields and global camera offsets.
- Palette and OAM transfers: 476 samples in memory copies, including 221 below
  `TransferPlttBuffer`. Skipping unchanged data needs reliable invalidation;
  adding a comparison can cost as much as these small copies.
- Sprite animation: 223 samples in `AnimateSprites`, plus its callbacks and
  affine helpers. Dummy callbacks and repeated affine work are candidates,
  but the zero-rotation experiment did not improve aggregate throughput.
- Software rendering: roughly 90 microseconds per rendered frame, hundreds
  of times the cost of a simulation frame. This matters for a frontend that
  renders every frame, although it does not contribute to the engine score.

These are measured hotspots or follow-up hypotheses, not claims that the
remaining work can safely be skipped.

## Software renderer text backgrounds

The baseline is commit `27f39a8fe`. Simulation costs about 0.2 microseconds
per frame and rendering about 92, so a frontend that draws every frame spends
over 99% of its engine time in `WasmRenderFrame`. With the render phases
marked `noinline`, sampling put 696 of 804 renderer samples in `RenderTextBg`.

Two facts about the Mudkip replay shaped the change:

- 81% of frames enable windows and alpha blending with no background as a
  blend source. Every background pixel loaded a window mask and ran blend
  checks that could not apply.
- Of 17,768 tile rows visited per frame, 48% were empty, 51% fully opaque,
  and about 1% mixed. 47% belonged to tiles with no drawn pixel at all.

The renderer now resolves each layer once per scanline. A line with one
window mask is hidden, drawn with plain stores, or sent to the old per-pixel
path. Alpha blending and lines with mixed masks keep the per-pixel path.
Brightness effects use a palette built for the line's BLDY. Each map row
also records which tile columns draw anything, and its eight scanlines visit
only those columns.

`--render-trace` renders all 27,229 replay frames, writes one display hash
per frame, and times only the renderer. Results are medians of three
alternating runs per binary.

| Measurement | Baseline us/frame | New us/frame | Change |
| --- | ---: | ---: | ---: |
| Replay trace | 97.54 | 53.76 | -44.9% |
| Overworld end state | 98.21 | 54.57 | -44.4% |
| Menu end state | 89.94 | 60.06 | -33.2% |
| Battle end state | 90.14 | 58.88 | -34.7% |

An earlier alternating round measured the trace at 93.35 before and 53.62
after (-42.6%). The engine score did not move: 7,279,124 before and
7,353,670 after.

The two traces are byte-identical, covering 7,373 distinct screens. The six
goldens and `make native-test` pass. In the browser, the 18 Mudkip replay
screenshots are byte-identical between the two renderers.

Rejected experiments, all with identical output:

- A branchless select for whole tile rows measured 74 against 67 for the
  branching loop at that stage. Transparent rows exit early, and that matters
  more than mispredictions.
- Removing every store from the tile plotter saved only 2 microseconds, so
  the cost is tile iteration and decode, not memory writes.
- Per-line resolution for sprites, a hoisted backdrop color, and a channel
  table for palette conversion together measured 53.43 against 53.62. That is
  within run-to-run drift of about 4%, so they were dropped.

Opaque tile rows now dominate. Backgrounds draw about 8,900 of them per
frame for 4,800 rows of screen, so occlusion between opaque layers is the
next candidate. It must keep alpha blending and sprite ordering exact.

```sh
build/native/pokeemerald-bench --script tools/wasm_replays/mudkip_starter.txt --render-trace build/native/perf/trace.txt
```

## Native OAM offset arithmetic

The baseline is commit `c505a2a3c`, using the same M3 Max, compiler, flags,
and no PGO. The OAM helper used a `uint32_t` loop index, so `i * 2` and
`i * 8` required 32-bit wraparound before being added to 64-bit pointers.
Changing the index to `size_t` removes that requirement. In the linked
`BuildOamBuffer`, Clang emits zero of the 17 offset masks present before.
The records written and the loop bounds are unchanged.

Initial measurements were inconclusive while another CPU-heavy application
was running. After it stopped, all measurements below used the normal
wall-clock benchmark, without profiling, compilation, clock substitutions,
or thread-priority changes during measurement. Each binary ran eight passes
per run, and run order alternated.

In a comparison of three candidate implementations, medians of three runs
were 7,459,235 frames/s for the baseline and 7,660,389 for the pointer-sized
index (+2.7%). Explicit four-record batching reached 7,682,285 and ARM64
interleaved stores reached 7,718,043. Those added only 0.3% and 0.8% over the
index change, so the retained implementation is the one-line index change.

A separate comparison after rebuilding the final source reproduced the gain.
These are medians of three alternating eight-pass runs per binary:

| Scenario | Baseline frames/s | Pointer-sized index frames/s | Change |
| --- | ---: | ---: | ---: |
| Overworld | 4,761,361 | 4,763,232 | +0.0% |
| Menu | 11,650,905 | 12,108,461 | +3.9% |
| Battle | 5,973,925 | 6,042,492 | +1.1% |
| Aggregate score | 7,467,773 | 7,643,286 | +2.4% |

The final native build and tests pass. The six unchanged framebuffer goldens,
sprite-sort checks, and determinism checks pass. All 27,229 render-trace
hashes match the baseline. The BIOS checks also pass with AddressSanitizer
and UndefinedBehaviorSanitizer, including all 16,641 OAM count/limit pairs
with unaligned buffers and guard bytes. The helper compiles for x86-64 and
ARM Cortex-A7; performance was measured only on this ARM64 Mac.

Raw results, disassembly, and traces are in the ignored `build/native/perf/`
directory. `ablation-*.json` records the candidate comparison and
`final-*.json` records the final rebuild comparison. The production benchmark
and goldens are unchanged.

## Larger uncapped simulation opportunities

The baseline is `64ec2a746`, including the pointer-sized OAM index. These
measurements use the same M3 Max and Apple clang 21. Each variant ran three
times with eight passes per run, reversing variant order in the middle round.
The table reports medians. Timing runs were sequential, without compilation,
sampling, or replay checks running alongside them.

| Scenario | Baseline frames/s | PGO frames/s | Change | OAM deferral prototype frames/s | Change |
| --- | ---: | ---: | ---: | ---: | ---: |
| Overworld | 4,783,482 | 5,428,552 | +13.5% | 5,862,163 | +22.6% |
| Menu | 12,121,763 | 13,142,371 | +8.4% | 18,108,492 | +49.4% |
| Battle | 6,047,018 | 6,765,291 | +11.9% | 7,892,442 | +30.5% |
| Aggregate score | 7,650,754 | 8,441,377 | +10.3% | 10,621,033 | +38.8% |

### Profile-guided compilation

The existing `native-pgo` target provides the immediate opportunity. An
isolated build trained on one benchmark pass and used the resulting profile
for the optimized build:

```sh
make -j8 native-pgo \
  NATIVE_BUILD_DIR=build/native/perf/opportunities/pgo-engine \
  NATIVE_PGO_PROFILE=build/native/perf/opportunities/pgo/native.profdata
```

All six unchanged goldens, determinism checks, and sprite-sort checks passed.
All 27,229 per-frame replay display hashes also matched the baseline. Build
warnings were the existing unaligned data relocation and constant-array
extension warnings; there were no other warnings.

Training and measurement use the same scenarios, so the 10.3% gain does not
establish performance across the whole game. No default build flags or
profile location changed. `make native-pgo` without those overrides trains
and enables PGO for subsequent desktop builds through the existing workflow.

The verified profile is now installed at the default local path,
`build/native/pgo/native.profdata`. The normal `native-bench`, `native-test`,
and `native-raylib` targets were rebuilt with it. Native tests, all six
goldens, determinism and sprite-sort checks, and all 27,229 replay hashes
pass. The profile is a generated local artifact; `make clean-native` removes
it, and `make native-pgo` recreates it. Activation checks are recorded in
`pgo-enable.log`, `pgo-enabled.json`, and `pgo-enabled-trace.txt` under the
opportunities directory.

### Defer final OAM preparation between displayed frames

An isolated diagnostic skipped `AddSpritesToOamBuffer`,
`CopyMatricesToOamBuffer`, and `LoadOam` for the first 1,000,002 measured
frames, then executed them normally on the final measured frame. It retained
sprite coordinate updates, sorting, animation, callbacks, sprite copy request
processing, palette uploads, and the rest of VBlank. The six benchmark
goldens and determinism checks still passed.

The 38.8% gain is evidence of potential savings, not a correct optimization.
A second diagnostic deferred OAM throughout the input replay and compared
each displayed frame against the normal trace:

| Render interval | Frames checked | Display hash mismatches |
| --- | ---: | ---: |
| Every 2 frames | 13,615 | 0 |
| Every 16 frames | 1,703 | 7 |
| Every 256 frames | 108 | 1 |

At interval 16, frames 26,368 through 26,464, spaced 16 apart, differed during
the starter-selection transition. Interval 256 differed at frame 26,368.
This prototype is not enabled in production. A correct design must preserve
the timing and contents of prepared and loaded OAM across callback changes,
direct OAM writes, and frames where ordinary preparation or loading stops.
Rebuilding only on a displayed frame is insufficient.

Follow-up instrumentation traced the failure to frame 26,360. The starter
callback changes `gMain.callback2`, then finishes its last `BuildOamBuffer`
call. Subsequent frames continue calling `LoadOam` without preparing a new
buffer. Skipping that last preparation loses the contents needed by the
next displayed frame. Deferral remains disabled; an endpoint-only benchmark
cannot validate it. The instrumented trace is `trace-oam-16.log` in the
opportunities directory.

An alternative cached the packed dummy OAM records by matrix bytes and dummy
attributes, then copied the inactive range in bulk. This retained every
preparation and load at its original time. All six goldens and all 27,229
replay hashes matched; the unaligned BIOS checks also passed under ASan and
UBSan. The first comparison lost 5.5% with the existing profile. After fresh
training and rebuilding all engine objects with the candidate's profile,
three alternating eight-pass runs still lost 3.5%: median aggregate
throughput fell from 8,441,531 to 8,146,227 frames/s. The cache is also
rejected. The production OAM implementation is unchanged.

The cache experiment, retraining script, timing results, and traces are
`cache-bios.c`, `retrain-cache.py`, `cache-trained-*.json`, and
`cache-trained-trace.txt` in the opportunities directory. Its full-profile
rebuild had no profile mismatch warnings.

Broader skipping also has explicit dependencies: field effects read
`sprite->oam.x/y`, `ReadPlttIntoBuffers` reads palette RAM back into game
buffers, and VBlank advances RNG and timers. Those operations cannot simply
be removed when a frame is not displayed.

### Batch frontend clock checks

A separate diagnostic kept the engine unchanged and checked
`CLOCK_MONOTONIC` after each batch of simulation calls. Median aggregate
scores were 7,062,731 frames/s for batches of 1, 7,556,594 for 16, and
7,661,826 for 256. Moving from 1 to 16 recovered 7.0%; moving from 16 to
256 added 1.4%. All benchmark correctness gates passed.

The Kindle max-speed loop now checks the clock every 16 frames, matching
Raylib. Frame limits are still checked after every frame. Compared with
checking every frame, a slice can run up to 15 extra simulation frames
before polling input. The full Cortex-A7 Linux cross-build passes. A focused
sanitizer check of the source loop passes for exact frame limits, deadline
boundaries, and frame-counter wraparound. The build log and check source
are `kindle-clock-build.log` and `kindle-clock-check.c` in the opportunities
directory.

The Mac measurements do not establish a gain or acceptable input latency on
Kindle hardware. This change does not improve the existing headless
benchmark, which already times the whole fixed batch without per-frame
clock checks.

Raw results, isolated diagnostic sources and binaries, profile data, and
replay comparisons are under the ignored `build/native/perf/opportunities/`
directory. The production engine and benchmark remain unchanged; desktop
builds now use the local PGO profile, and the Kindle frontend batches clock
checks.

## ARM64 OAM byte shuffles

OAM preparation and loading still run on their original frames. The native
ARM64 helper now expands eight matrix halfwords into eight OAM records with
byte shuffles. It fills inactive records in bulk and preserves the attribute
bytes beyond the OAM limit. Scalar loops handle active records and partial
batches. Other native architectures retain the previous implementation.
No original game source, browser code, framebuffer goldens, or timing rules
changed.

A plain C bulk-write prototype roughly halved the isolated helper time but
lost 0.9% in the full engine after fresh PGO training. Inspection of the
linked code showed that PGO already vectorized and unrolled the old loops.
Larger plain C batches, vector widening, NEON interleaved writes, and moving
the helper out of line did not establish a gain. Byte shuffles avoid the
repeated widening and shifting needed to place each matrix component in the
last halfword of an OAM record.

The final comparison used the production binary after `make native-pgo`,
against a preserved build of `a1d5044af` with its previous trained profile.
Both use Apple clang 21.0.0, `-O3 -flto`, and PGO on the same M3 Max. Results
are medians of three alternating eight-pass runs per binary, using the
unchanged replay, warmup, and measurement counts. Compilation and other
validation had finished before these runs.

| Scenario | Previous frames/s | Byte-shuffle frames/s | Change |
| --- | ---: | ---: | ---: |
| Overworld | 5,294,999 | 5,391,560 | +1.8% |
| Menu | 12,797,090 | 13,209,970 | +3.2% |
| Battle | 6,516,275 | 6,549,248 | +0.5% |
| Aggregate score | 8,202,788 | 8,383,593 | +2.2% |

All six unchanged goldens, determinism checks, and sprite-sort checks pass.
The final binary matches all 27,229 baseline replay display hashes,
including the starter transition that exposed the deferral bug. The existing
16,641 count/limit cases, unaligned buffers, and guard bytes pass under ASan
and UBSan. The x86 fallback BIOS checks also pass under Rosetta.

`make native-pgo`, `make native-test native-raylib`, and `make native-kindle`
pass. The benchmark/profile rebuild has no profile mismatch warnings. The
Raylib build discards the benchmark's incompatible `main` profile; the
engine uses the newly trained profile. Other desktop warnings are the
existing unaligned data relocation and constant-array extension warnings.

The measured gain applies to these ARM64 headless scenarios with PGO. It
does not establish a whole-game, displayed-FPS, browser, or Kindle gain.
The ARM64 path preserves every OAM preparation and load; the rejected
cache and deferral paths remain disabled.

Sources for discarded prototypes, raw timing results, build logs, and replay
traces are in the ignored `build/native/perf/oam-bulk/` directory. The final
measurements are `baseline-production-*.json`, `final-production-*.json`, and
`production-summary.json`; correctness evidence includes `final-trace.txt`,
`final-targets.log`, and `final-kindle.log`. The refreshed default profile is
local at `build/native/pgo/native.profdata` and is recreated by
`make native-pgo`.

## Two-input ARM64 OAM shuffles

The native helper now selects attribute bytes and matrix bytes in the same
shuffle. Inactive records take their attributes from the dummy OAM record;
records beyond the OAM limit take them from the current destination. The
linked ARM64 code uses two-register `TBL` instructions, removing the separate
vector mask and OR operations. Active records, partial batches, and the
non-ARM64 fallback retain their scalar paths. Every OAM preparation and load
still runs on its original frame.

This round tested eight candidates: two-input shuffles, `TBX` merges, a
`TBX` tail with the previous fill loop, compact loops, two-way loop unrolling,
direct OAM copies, compare-before-copy, and inline OAM copies. The two-input
shuffle was selected for final validation. The copy and loop experiments
did not establish a useful gain. Prototypes remain outside production code.

The final comparison used the production binary after fresh `make native-pgo`
training against the preserved `fab610cf7` binary and its previous profile.
Both use Apple clang 21.0.0, `-O3 -flto`, and PGO on the same M3 Max. These
are medians of sixteen alternating eight-pass runs per binary with the
unchanged replay, warmup, measurement count, and framebuffer goldens.
Compilation and other validation finished before timing.

| Scenario | Previous frames/s | Two-input frames/s | Change |
| --- | ---: | ---: | ---: |
| Overworld | 5,489,075 | 5,479,451 | -0.2% |
| Menu | 13,556,864 | 13,634,380 | +0.6% |
| Battle | 6,782,326 | 6,876,432 | +1.4% |
| Aggregate score | 8,606,554 | 8,661,705 | +0.6% |

The candidate scored higher in 15 of 16 paired runs. The mean paired score
change was +0.86%; a paired bootstrap interval was +0.32% to +1.47% at 95%.
Background CPU activity caused visible timing variation. The supported gain
is small and concentrated in menu and battle; overworld is within that
variation. This does not establish a whole-game, displayed-FPS, browser, or
Kindle performance improvement.

All six unchanged goldens and the determinism, progression, and sprite-sort
checks pass. All 27,229 replay display hashes match the previous renderer,
including the starter transition. The 16,641 count/limit combinations,
unaligned buffers, and guard bytes pass under ASan and UBSan. The x86 fallback
checks pass under Rosetta. `make native-pgo`, `make native-test native-raylib`,
and `make native-kindle` pass. No original game source or goldens changed.
The PGO benchmark rebuild has no profile mismatch warnings; the Raylib build
continues to discard the benchmark's incompatible frontend `main` profile.

Sources, raw measurements, profiles, and logs are in the ignored
`build/native/perf/oam-next/` directory. Final evidence includes
`baseline-production-*.json`, `final-production-*.json`,
`production-summary.json`, `final-artifacts.json`, `final-buildoam.s`,
`final-trace.txt`, `final-targets.log`, and `final-kindle.log`. The default
profile at `build/native/pgo/native.profdata` is freshly trained for this code.
