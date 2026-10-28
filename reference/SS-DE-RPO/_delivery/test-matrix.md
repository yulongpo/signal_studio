# SS-DE-RPO continuation acceptance matrix

PASS below always has the stated scope. The entire case remains OPEN until all requirements in 06_VALIDATION_MATRIX.md are supported by final-version evidence.

| Case | Scope currently evidenced | Completion |
|---|---|---|
| R-01 | Baseline logic + real original IQ Sample 20/20 with 8 GiB authorization | OPEN: final implementation |
| R-02 | Budget propagation matrix not yet executed | OPEN |
| R-03 | Overflow/low-resource matrix not yet executed | OPEN |
| R-04 | Existing logical OFF/quota regressions; new real raw-OFF 20/20 cold x3 | OPEN: final implementation |
| R-05 | Existing short-volume logic | OPEN: final implementation and retention |
| R-06 | Original IQ Sample 20/20 | OPEN: 363/2044 and final implementation |
| R-07 | Existing Golden/time strategy evidence to rerun | OPEN |
| R-08 | Baseline offscreen repeated-submit regression | OPEN: native and final implementation |
| R-09 | Preparing cancellation implementation delegated to C/D | OPEN |
| R-10 | Fault matrix delegated to G | OPEN |
| R-11 | New Debug original IQ 20/20 formal transaction and reopen PASS | OPEN: 159/363/2044 |
| R-12 | Existing durable/non-durable logic; real raw-OFF 20/20 formal commit PASS | OPEN: final implementation |
| R-13 | Current UX-R6 contract preserved | OPEN: final UI/transaction regression |
| R-14 | Existing fault checks to rerun | OPEN |
| R-15 | Baseline existing adapted live GPU suite running; new F work pending | OPEN |
| R-16 | Deterministic interval-index implementation delegated to E | OPEN |
| R-17 | Long Sample implementation/tests delegated to B | OPEN |
| R-18 | Preparation cache/cancellation delegated to D | OPEN |
| R-19 | Cold pipeline x3 measured; not UI feedback p95 | OPEN |
| R-20 | Complete Debug/Release/native regression pending | OPEN |

Initial Release RPO test attempt omitted the logic executable and recorded NOT RUN. After building that executable, the second attempt passed 2/2; both logs are retained. An invalid hand-created request added a runtime-only field and was correctly rejected; corrected cold runs use the public request codec shape. Neither failed harness preparation is classified as a product regression.
