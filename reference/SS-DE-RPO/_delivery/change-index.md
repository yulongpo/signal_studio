# SS-DE-RPO continuation change index

Baseline: 7220b2e. Existing delivery and old independent reviews are preserved under out/tasks/ss-de-rpo/N0/legacy-delivery.

| Commit | Change | Evidence |
|---|---|---|
| f7d38f0 | Freeze continuation interfaces, launch independent Grok Fast/xhigh sessions, preserve cold baseline | BASELINE_COLD.json, original transcripts in ignored task output |
| f671925 | Add opt-in isolated formal project transaction/reopen to detection benchmark | FORMAL_PILOT.json; Debug real IQ Sample 20/20, formal commit/reopen PASS |

The benchmark does not edit the input/source project. Without --formal-commit, its existing pipeline-candidate behavior is preserved. With the option, PASS additionally requires durable comparison, atomic commit to a new isolated .ssproj, and reopened committed slot identity.
