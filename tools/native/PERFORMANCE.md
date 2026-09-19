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
