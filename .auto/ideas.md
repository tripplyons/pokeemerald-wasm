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

## PROVEN BIG WIN — pass fusion (iteration 2, verified +8.9% over iter1)
- [x] Fusing the 3 pre-sort passes over gSprites (UpdateOamCoords + BuildSpritePriorities
      + sort-key precompute) into ONE loop = +8.9% verified (clean A/B, zero distribution
      overlap). Cache traffic over the large struct Sprite[64] was a real bottleneck.
      Packed-u32 sort key + single compare on top (committed d5aedb18d).
- KEY INSIGHT: redundant PASSES over large arrays (not just redundant ops) are the lever.
      Look for more multi-pass patterns over gSprites / gOamMatrices / palette / tile buffers.
- Next candidates: AnimateSprites is a separate gSprites pass but in a different phase (CB1)
      than BuildOamBuffer (VBlank) — cannot fuse across phases. Within BuildOamBuffer the
      pre-sort fusion is captured; AddSpritesToOamBuffer runs post-sort (sorted order).

## PROVEN BIG WIN — indirect-dispatch elimination (iteration 3, +8.8%)
- [x] AnimateSprites/AnimateSprite dispatched through sAnimFuncs/sAffineAnimFuncs function-pointer
      tables, blocking inlining of ContinueAnim/ContinueAffineAnim. Replacing with direct calls
      (branch on animBeginning/affineAnimBeginning) + hoisting gAffineAnimsDisabled = +8.8%.
- KEY INSIGHT: function-pointer dispatch tables in hot loops block inlining; specialize the
      common case with direct calls. Look for more: sAnimCmdFuncs (anim command interpreter),
      sAffineAnimCmdFuncs (AffineAnimCmd_end was ~4% alone!).
- NEGATIVE: hand-inlining AddSpritesToOamBuffer's AddSpriteToOamBuffer call HURT (-5.1%) —
      it blocked AddSubspritesToOamBuffer inlining / worsened layout. Don't hand-inline there.
