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

## EXHAUSTION ASSESSMENT (after iteration 5)
Cumulative verified wins (~+22% over original 2.01M baseline):
  1. SortSprites adjusted-Y precompute (+2.6%, A/B verified)
  2. 3-way pre-sort pass fusion + packed-u32 sort key (+8.9%, A/B verified, zero overlap)
  3. AnimateSprites indirect-dispatch -> direct calls (+8.8%, two impls agree)
NEGATIVE / NOISE results (do NOT retry):
  - Command-interpreter dispatch (sAnimCmdFuncs/sAffineAnimCmdFuncs) -> switch: NOISE (cmd
    interpreters fire rarely, delay-gated; anim switch even hurt).
  - Hand-inlining AddSpritesToOamBuffer's AddSpriteToOamBuffer call: HURT -5.1%.
  - __attribute__((flatten)) on hot fns: HURT -5..-7% (icache / defeats wasm optimizer).
UNSAFE (do NOT do without broad changes — would overfit benchmark):
  - AffineAnimCmd_end redundant-recompute skip: gOamMatrices has scattered DIRECT writers in
    pokemon_animation.c, battle_anim_{mons,flying,electric}.c, roulette.c. A dirty-flag would
    need all those sites; missing one = silent general-correctness bug not caught by benchmark.
REMAINING (last safe structural idea, marginal ~+1-2%):
  - Sort-skip-when-unchanged: cache prev sSortKey[64]; if identical, sSpriteOrder is already
    sorted (persists across frames) -> skip insertion sort. Detect change inside the fused
    pre-sort pass (no extra pass). Safe. Likely near noise floor.
BuildOamBuffer (~46%) is now mostly necessary OAM-emission work; resistant to safe opts.

## PROVEN BIG WIN — sort-skip-when-unchanged (iteration 6, +5.9%)
- [x] sSpriteOrder persists across frames (only SortSprites modifies it). If this frame's
      packed sort keys == last frame's, the order is already correct -> skip the insertion sort
      entirely. Detect change INSIDE the fused pre-sort pass (compare key vs sPrevSortKey[i],
      set sSortDirty; update sPrevSortKey[i] inline -> no extra pass, no memcpy). Cumulative ~+30%.
- KEY INSIGHT: the sort's dependent random-access key lookups (sSortKey[sSpriteOrder[i]]) were
      costlier than the profile implied. Reusing persistent derived state when inputs are stable
      is a strong lever. NOTE: memcmp/memcpy UNAVAILABLE in sprite.c wasm build (arm0 crashed).
- Look for more persistent-state-reuse: CopyMatricesToOamBuffer (skip if matrices unchanged),
      but most other BuildOamBuffer work produces immediately-consumed output (can't skip).

## PROVEN HUGE WIN — gate sort-key to emitted sprites (iteration 8, +22.5%, cumulative ~+59%)
- [x] Computing sort keys for ALL 64 sprites (incl invisible-but-active ones whose positions
      update off-screen every frame) kept sSortDirty permanently TRUE -> the iter6 sort-skip
      NEVER triggered. Gating key computation to inUse&&!invisible (the actually-emitted set)
      makes the sort-skip effective -> insertion sort skipped on most frames. +22.5%.
- Also dropped the dead sSpritePriorities write (never read; packed sort uses sSortKey).
- KEY INSIGHT: persistent-state skips (sort-skip) are only as good as the change-detection;
      pollution from non-output-affecting state (invisible sprites) silently disables them.
      Arm1 (keys for inUse incl invisible) gained less (+15.9%) -> confirmed invisible sprites
      were the polluters.

## PROVEN WIN — AffineAnimCmd_end lazy cache (iteration 9, +1.9%, cumulative ~+62%)
- [x] AffineAnimCmd_end recomputed the matrix (2 divisions + trig) every frame for ended affine
      sprites. Lazy cache maintained ONLY in the end path (NOT the active ApplyAffineAnimFrame...
      path, which is what made iter7's version regress): when (xScale,yScale,rotation) unchanged,
      reuse cached matrix; output-check skips even the copy when gOamMatrices already matches.
      Always overwrites gOamMatrices on recompute -> preserves "affine state wins over external
      writers" semantics (safe vs battle_anim_*/pokemon_animation direct writers). +1.9%.
- DIMINISHING RETURNS: wins went +2.6,+8.9,+8.8,+5.9,+22.5,+1.9. Remaining hotspots are necessary
      OAM emission (BuildOamBuffer ~47%, sort now skipped) + gameplay callbacks (untouchable).

## Iteration 10 — NEGATIVE (confirms matrix/output-skip exhaustion)
- CopyMatricesToOamBuffer output-comparison skip: REGRESSED -3.6%. 4 reads/matrix to skip 4
  writes is a net loss (both touch same cache lines). External writers (battle_anim_*.c write
  gOamMatrices directly) also block any dirty-flag variant. CopyMatrices is irreducible.
- AffineAnimCmd_end local-pointer hoist: neutral (-0.1%); compiler already CSEs the indexing.
- PLATEAU: wins +2.6,+8.9,+8.8,+5.9,+22.5,+1.9 then iter10 all negative/neutral. At safe optimum
  (~+62% cumulative). Remaining: BuildOamBuffer 47% = necessary OAM emission; gameplay untouchable.

## Iteration 11 — NEGATIVE (emission inner loop is compiler-optimal)
- Hoist gOamLimit to local: -1.1% (compiler LICM already hoists it).
- Load struct Subsprite once/iter: -0.9% (compiler already register-loads the 4-byte struct).
- Both: -0.4%. The AddSubspritesToOamBuffer inner loop cannot be hand-improved.

## FINAL VERIFIED STATE (clean rebuild x2): fps = 3,262,521 / 3,262,977 (±0.01%)
- +62.0% over original baseline 2,014,781. Golden hashes + determinism gate PASS on every run.
- Per-scenario: overworld ~2.43M, menu ~4.52M, battle ~2.81M (baseline ~1.6M/2.6M/1.6M).
- EXHAUSTION CONFIRMED: iterations 10 & 11 produced ZERO wins (all neutral/negative).
  Every remaining hotspot is either necessary work (BuildOamBuffer OAM emission 47%,
  gameplay callbacks 14%), compiler-optimal (emission loop), external-writer-blocked
  (matrix/palette dirty-skips), or ~0-net (palette compare costs ~= the copy).
- Safe provably-equivalent surface is at its optimum. Further gains would require either
  unsafe transforms (violating the equivalence mandate) or changing the rendering approach
  (out of scope). RECOMMEND STOPPING unless a fundamentally new angle is identified.

## PROVEN PATTERN — skip settled-state re-run handlers (iterations 12 & 13)
- [x] iter12 (+1.2%): AnimateSprites called BeginAffineAnim/ContinueAffineAnim for EVERY active
      sprite; non-affine sprites (majority) just early-out inside. Hoist (affineMode & ON_MASK)
      check into AnimateSprites to skip the call. affineDisabled-first order best.
- [x] iter13 (+1.7%): ended anims re-run ContinueAnim -> AnimCmd_end (animCmdIndex++ then -- ,
      animEnded=TRUE) every frame — a no-op oscillation. Skip ContinueAnim when animEnded
      (animBeginning path -> BeginAnim clears animEnded, so restarts safe). Ended-first branch
      layout `if (!animBeginning && animEnded) {} else if (animBeginning) BeginAnim else
      ContinueAnim` beats a plain else-if (combined check short-circuits static sprites).
- KEY INSIGHT: sprites in a SETTLED state (ended anim/affine, non-affine) re-run no-op handlers
      every frame. Hoist the settling condition into the caller to skip the call. Look for more:
      affineAnimEnded skip in ContinueAffineAnim callers, tileset/palette idle, object-event idle.
- Cumulative ~+67% (3.36M vs 2.01M baseline). Clean-rebuild verified each step.
