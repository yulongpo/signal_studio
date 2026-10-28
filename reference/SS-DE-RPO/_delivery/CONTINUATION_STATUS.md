# SS-DE-RPO continuation status

## N0 baseline

Baseline HEAD: 7220b2e295963288cb5d11e76946bb77646be0b0. Remote was read and matches. Existing modified/untracked user material is preserved; its inventory and old delivery copies are in `out/tasks/ss-de-rpo/N0/`.

The initial RPO tests and frozen Release baseline are preserved. The calibrated 7220b2e branch adds observation and the common native harness only (4371940 / e52c3a9), without the new preparation/cache/resource scheduling changes. Pilot timings are not final comparisons.

Grok CLI 1.0.70 supports model `grok-4.7-build-fast` and effort `xhigh`, labelled Extra High by its local catalog. Roles, actual session identities, patches and checks will be recorded as they execute.

## Remaining nodes

| Node | State | Exit evidence |
|---|---|---|
| N0 contract / baseline | COMPLETE | original7220b2e preserved; reproducible observer patch; final native baseline cold/resident complete |
| N1 resource / Sample | IMPLEMENTED_AND_CHECKED | live ResourceDecision, budget propagation, retained allocation caps, Golden/long, bounded 60 s synthetic execution at 20 MS/s with explicit measured-coarse fallback |
| N2 start / cancellation / timeline | IMPLEMENTED_AND_CHECKED | provisional cancellation, post-atomic queue/cancel, parent-clock timeline; notice/context fix e5ae911 passes both start suites |
| N3 tracking / publication / preparation | IMPLEMENTED_AND_CHECKED | deterministic interval oracle, real 159/363/2044 Sample formal commit/reopen, bounded SourcePlan reuse, one-window startup overlap |
| N4 GPU / full regression / native | COMPLETE_AFTER_H2_CORRECTION | final07 all22gates PASS; Debug60/60 1052.54s, Release60/60 294.04s; fresh original20/159/363/2044; package and native performance03 pass |
| N5 review / push | EXTERNAL_H3_EXACT_HEAD_GATE | H1/H2 FAIL preserved; fresh H3 starts only after final evidence commit; no push yet |

Implementation details and correction evidence are in REVIEW_FIX_CLOSURE.md and agents/C/ADDITIONAL_UI_FIXES.md. H2 rejected83e365f; C4 fixed source-load completion, C5/C6 fixed narrowband/selection writes, and main4898fa4 fixed modal lifetime, save/publication coordination and shutdown. Directed integration, fresh full qualification and native performance03 have passed. Cold/native formal time decreases18.37%, resident23.90%; all14runs including warmups succeeded,8projects/9Algorithm geometries exactly equal. Final verdict and remote SHA live outside Git.
