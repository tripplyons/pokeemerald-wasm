# Ideas backlog (grounded in profiling; BuildOamBuffer = ~50% of frame time)

## High priority — sprite.c / BuildOamBuffer
- [ ] SortSprites: precompute the per-sprite adjusted-Y sort key once (the
      affineMode==DOUBLE && size==3 shape Y-adjustment is recomputed on EVERY
      O(n^2) comparison today). The comparison keys (priority, adjustedY) are
      invariant during the sort, so precomputing adjustedY[MAX_SPRITES] and
      looking it up preserves the EXACT insertion-sort order while cutting
      redundant work. Provably equivalent; hashes gate it.
- [ ] BuildSpritePriorities computes priority for all 64 slots; SortSprites then
      reads sSpritePriorities. Fuse/avoid recomputation; ensure unused sprites'
      keys don't perturb order (they still occupy sSpriteOrder slots — preserve).
- [ ] UpdateOamCoords + BuildSpritePriorities both loop all 64 sprites separately;
      fuse into one pass (watch: BuildSpritePriorities runs for ALL sprites incl.
      unused, UpdateOamCoords only inUse&&!invisible — preserve exact effects).
- [ ] AddSpritesToOamBuffer / AddSpriteToOamBuffer inner work: inspect for
      redundant per-sprite recomputation.

## Medium priority
- [ ] AnimateSprites (~13%): the per-sprite callback dispatch + AnimateSprite;
      look for redundant affine/matrix recalcs. Behavior-sensitive — verify hashes.
- [ ] Affine anim path (~8%): ContinueAffineAnim / AffineAnimCmd processing.
- [ ] VBlankIntr / VBlankCB_Field / VBlankCB_Battle: redundant register copies.
- [ ] TransferPlttBuffer (~2%): palette transfer each frame; check for full-copy
      vs dirty-range opportunities (must preserve exact palette output).

## Low priority / likely noise (deprioritize)
- [ ] Compiler flag tweaks (proven within noise — skip unless paired with code).
- [ ] native_engine.c host shims (proven not the bottleneck).
- [ ] src/wasm_display.c renderer (OUTSIDE timed region — will not move metric).

## Guardrails reminder
Every idea must preserve the three 1M-frame golden trajectories. When in doubt
about equivalence, prefer the smaller, obviously-equal transform.
