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
