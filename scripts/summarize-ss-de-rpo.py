"""Summarize SS-DE-RPO matrix and benchmark launches.

Missing numbers stay null. A failed run is not a speedup denominator.
Warm speedup requires residentApplication and distinct Worker process ids.
Three fresh processes are not warm.
"""
import argparse
import json
import statistics
from pathlib import Path


BUCKETS = {
    "synthetic-iq": "syntheticIq",
    "original-iq": "originalIq",
    "original-iq-source-plan": "originalIq",
    "fixture-worker": "fixtureWorker",
    "tensorrt": "tensorrt",
    "qrhi": "qrhi",
    "software-fallback": "softwareFallback",
}


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def linear_stats(values):
    clean = []
    for value in values:
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            continue
        if value != value:
            continue
        clean.append(float(value))
    clean.sort()
    if not clean:
        return {"available": False, "n": 0, "minimum": None, "maximum": None,
                "mean": None, "p50": None, "p95": None}

    def quantile(q):
        position = q * (len(clean) - 1)
        lower = int(position)
        upper = min(lower + 1, len(clean) - 1)
        return clean[lower] + (clean[upper] - clean[lower]) * (position - lower)

    return {"available": True, "n": len(clean), "minimum": clean[0], "maximum": clean[-1],
            "mean": statistics.mean(clean), "p50": quantile(0.5), "p95": quantile(0.95)}


def refuse_main_out(path):
    full = str(Path(path).resolve()).replace("/", "\\").lower()
    banned = "d:\\project\\signal_studio_rebuild\\out"
    if full == banned or full.startswith(banned + "\\"):
        raise SystemExit(f"refusing to write the main worktree out directory: {path}")


def finite_elapsed(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    if value != value:
        return None
    return float(value)


def measurement_elapsed(path):
    if not path or not Path(path).is_file():
        return None
    try:
        return finite_elapsed(read_json(path).get("elapsedMs"))
    except (OSError, json.JSONDecodeError):
        return None


def speedup(baseline_values, new_values, comparable, reason):
    if not comparable:
        return {"available": False, "ratio": None, "reason": reason}
    base = linear_stats(baseline_values)
    new = linear_stats(new_values)
    if not base["available"] or not new["available"] or not new["mean"]:
        return {"available": False, "ratio": None,
                "reason": "one side has no finite elapsed samples; missing data was not replaced with 0"}
    return {"available": True, "ratio": base["mean"] / new["mean"],
            "baselineMeanMs": base["mean"], "newMeanMs": new["mean"],
            "reason": "mean(baseline) / mean(new) over passed runs only"}


def empty_classes():
    return {name: [] for name in
            ("syntheticIq", "originalIq", "fixtureWorker", "tensorrt", "qrhi",
             "softwareFallback", "unclassified")}


def stamp_or_none(value):
    # A missing pipeline or business stamp stays null. One is never copied
    # into the other.
    if value is None or value == "":
        return None
    return value


def add_run(classes, run):
    bucket = BUCKETS.get(run.get("evidenceClass") or "", "unclassified")
    classes[bucket].append({
        "pass": run.get("pass") is True,
        "elapsedMs": measurement_elapsed(run.get("measurement")),
        "residentApplication": run.get("residentApplication") is True,
        "newProcessPerRun": run.get("newProcessPerRun") is True,
        "workerProcessIds": run.get("workerProcessIds") or [],
        "status": run.get("status"),
        "t_pipeline_terminal": stamp_or_none(run.get("t_pipeline_terminal")),
        "t_terminal": stamp_or_none(run.get("t_terminal")),
    })


def summarize(args):
    refuse_main_out(args.output)
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    matrices = [{"path": str(Path(path).resolve()), "payload": read_json(path)}
                for path in args.matrix]
    launches = []
    classes = empty_classes()
    warm_blocks = []
    for path in args.benchmark:
        payload = read_json(path)
        launches.append(str(Path(path).resolve()))
        for run in payload.get("coldRuns") or []:
            add_run(classes, run)
        if payload.get("warm"):
            warm_blocks.append(payload["warm"])

    baseline = read_json(args.baseline) if args.baseline else None
    baseline_elapsed = []
    baseline_failed = False
    if baseline:
        for run in baseline.get("runs") or []:
            elapsed = finite_elapsed(run.get("elapsedMs"))
            passed = run.get("pass") is True and run.get("lifecycle") in (None, "Completed")
            if not passed or elapsed is None:
                baseline_failed = True
                continue
            baseline_elapsed.append(elapsed)

    tensorrt = classes["tensorrt"]
    passed_cold = [row["elapsedMs"] for row in tensorrt
                   if row["pass"] and isinstance(row["elapsedMs"], float)]
    cold_failed = any(not row["pass"] or row["elapsedMs"] is None for row in tensorrt)
    cold_relabelled = any(row["residentApplication"] for row in tensorrt)
    if not tensorrt or cold_failed or cold_relabelled or baseline_failed or not baseline_elapsed:
        cold_comparable = False
        cold_reason = "cold speedup needs a passed baseline and passed new TensorRT cold runs with finite elapsedMs"
    else:
        cold_comparable = True
        cold_reason = "cold fresh-process comparison; not a warm resident comparison"

    warm_ids = []
    for block in warm_blocks:
        warm_ids.extend(str(item) for item in (block.get("workerProcessIds") or []))
    warm_distinct = len(set(warm_ids)) >= 3 and len(warm_ids) >= 3
    warm_ok = bool(warm_blocks) and warm_distinct and all(
        block.get("residentApplication") is True and block.get("status") == "PASS"
        and block.get("relabelledFreshProcess") is not True
        and block.get("newProcessPerRun") is not True
        for block in warm_blocks)
    warm_reason = ("resident warm comparison" if warm_ok else
                   "warm speedup requires one resident --resident-repeat parent and distinct Worker process ids; a new process is not warm")

    summary = {
        "schema": "signal-studio-ss-de-rpo-summary-v1",
        "percentileMethod": "linear interpolation; null when n=0; no zero-fill",
        "matrices": [{"path": item["path"], "schema": item["payload"].get("schema"),
                      "allowGpu": item["payload"].get("allowGpu"),
                      "nativeR19Pass": item["payload"].get("nativeR19Pass", False)}
                     for item in matrices],
        "launches": launches,
        "baseline": None if baseline is None else {
            "schema": baseline.get("schema"),
            "applicationCold": baseline.get("applicationCold"),
            "osCacheState": baseline.get("osCacheState"),
            "elapsed": linear_stats(baseline_elapsed),
            "failedRunExcluded": baseline_failed,
        },
        "evidence": {key: {"n": len(rows), "elapsed": linear_stats(
            [row["elapsedMs"] for row in rows if isinstance(row["elapsedMs"], float)])}
                     for key, rows in classes.items()},
        "coldSpeedup": speedup(baseline_elapsed, passed_cold, cold_comparable, cold_reason),
        "warmSpeedup": speedup(baseline_elapsed, [], False if not warm_ok else False, warm_reason),
        "boundaries": {
            "syntheticIq": "synthetic IQ is not original IQ",
            "originalIq": "manifest SourcePlan or the recorded real wav request",
            "fixtureWorker": "fixture worker is not TensorRT",
            "tensorrt": "true TensorRT forward only when the launcher evidenceClass is tensorrt",
            "qrhi": "native QRhi display, not offscreen Qt",
            "softwareFallback": "software renderer, recorded separately from QRhi",
            "offscreenPaint": "offscreen visible-paint p95 is not native R-19 PASS",
        },
    }
    # Warm timings are absent until a resident harness records them. Do not
    # reuse cold elapsedMs as the warm side.
    if warm_ok:
        summary["warmSpeedup"] = {"available": False, "ratio": None,
                                  "reason": "resident flags passed but this launcher has no separate warm elapsed samples"}
    output.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"output": str(output), "coldSpeedup": summary["coldSpeedup"]["available"],
                      "warmSpeedup": summary["warmSpeedup"]["available"]}, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--benchmark", action="append", default=[])
    parser.add_argument("--matrix", action="append", default=[])
    parser.add_argument("--baseline")
    parser.add_argument("--output", required=True)
    summarize(parser.parse_args())


if __name__ == "__main__":
    main()
