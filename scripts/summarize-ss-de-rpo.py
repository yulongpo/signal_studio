"""Compare real cold/resident launches without filling absent stages or times.

Resident samples require one parent, an excluded warmup and distinct Workers.
Host stages may overlap. GPU memory samples cover the whole device/display.
"""
import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def number(value):
    if isinstance(value, bool):
        return None
    try:
        result = float(value)
        return result if math.isfinite(result) else None
    except (TypeError, ValueError):
        return None


def stats(values):
    clean = sorted(v for item in values if (v := number(item)) is not None)
    if not clean:
        return {"n": 0, "mean": None, "minimum": None, "maximum": None, "p50": None, "p95": None}
    def quantile(q):
        position = q * (len(clean) - 1)
        lower = int(position)
        upper = min(lower + 1, len(clean) - 1)
        return clean[lower] + (clean[upper] - clean[lower]) * (position - lower)
    return {"n": len(clean), "mean": statistics.mean(clean), "minimum": clean[0],
            "maximum": clean[-1], "p50": quantile(.5), "p95": quantile(.95)}


def output_path(path):
    result = Path(path).resolve()
    root = Path(__file__).resolve().parents[1] / "out/tasks/ss-de-rpo"
    if not result.is_relative_to(root.resolve()):
        raise SystemExit(f"output must be below {root}")
    result.parent.mkdir(parents=True, exist_ok=True)
    return result


def signature(launch, measurement):
    run = measurement.get("run", {})
    request = run.get("request", {})
    source = dict(request.get("sourceIdentity", {}))
    for key in ("projectGeneration", "fileId"):
        source.pop(key, None)
    if source.get("path"):
        source["path"] = source["path"].replace("\\", "/").casefold()
    profile = request.get("profile", {})
    return {
        "configuration": launch.get("configuration"), "native": launch.get("native", False),
        "device": run.get("device"), "runtime": run.get("runtime"), "source": source,
        "modelSha256": request.get("modelSha256"),
        "profile": {k: v for k, v in profile.items() if k not in ("status", "name")},
        "parameters": request.get("parameters"), "requestedRange": request.get("requestedRange"),
        "contextRange": request.get("contextRange"), "spectrumMode": request.get("spectrumMode"),
        "workingSetBudgetBytes": request.get("workingSetBudgetBytes"),
        "evidenceBudgetBytes": request.get("evidenceBudgetBytes"),
        "retainRawModelOutput": request.get("retainRawModelOutput"),
        "completedWindows": run.get("completedWindows"), "eligibleWindows": run.get("eligibleWindows"),
        "dspRoute": run.get("performance", {}).get("sourceStages", {}).get("gpuDsp", {}).get("stftRoute")}


def resource_summary(path):
    if not path or not Path(path).is_file():
        return None
    groups, private, cpu = defaultdict(float), defaultdict(float), defaultdict(float)
    gpu, utilization, free = [], [], []
    with Path(path).open(encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            stamp = row["utc"]
            groups[stamp] += number(row.get("workingSet64")) or 0
            private[stamp] += number(row.get("privateMemorySize64")) or 0
            pid = row.get("processId")
            cpu[pid] = max(cpu[pid], number(row.get("totalProcessorTimeMs")) or 0)
            gpu.append(number(row.get("gpuUsedMiB")))
            utilization.append(number(row.get("gpuUtilizationPercent")))
            free.append(number(row.get("volumeAvailableBytes")))
    free = [v for v in free if v is not None]
    return {"path": str(Path(path).resolve()), "sampleCount": len(groups),
            "processTreeSampledRssPeakBytes": max(groups.values(), default=None),
            "processTreeSampledPrivatePeakBytes": max(private.values(), default=None),
            "observedProcessCpuMs": sum(cpu.values()) if cpu else None,
            "wholeDeviceUsedMiB": stats(gpu), "wholeDeviceUtilizationPercent": stats(utilization),
            "wholeVolumeFreeMinBytes": min(free, default=None),
            "wholeVolumeObservedFreeRangeBytes": max(free) - min(free) if free else None}


def load_sample(launch, row):
    path = row.get("measurement")
    measurement = read_json(path) if path and Path(path).is_file() else {}
    run = measurement.get("run", {})
    passed = (row.get("pass") is True and measurement.get("pass") is True
              and run.get("lifecycle") == "Completed"
              and measurement.get("formalCommitPerformed") is True
              and measurement.get("formalProjectReopened") is True
              and run.get("completedWindows") == run.get("eligibleWindows")
              and number(measurement.get("elapsedMs")) is not None)
    performance = run.get("performance", {})
    marks = {m["name"]: int(m["steadyNs"]) for m in performance.get("timeline", {}).get("marks", [])}
    origin = number(measurement.get("clickSteadyNs"))
    offsets = {k: (v - origin) / 1e6 for k, v in marks.items()} if origin else {}
    return {"pass": passed, "measurement": path, "elapsedMs": measurement.get("elapsedMs"),
            "clickToFirstInferenceMs": measurement.get("clickToFirstInferenceMs"),
            "clickToFormalMs": measurement.get("clickToFormalMs"), "signature": signature(launch, measurement),
            "timelineFromClickMs": offsets,
            "pipelineStages": {k: v.get("totalMs") for k, v in performance.get("pipelineStages", {}).items()},
            "sourceStages": {k: v.get("totalMs") for k, v in performance.get("sourceStages", {}).items()
                             if isinstance(v, dict) and "totalMs" in v},
            "tensorRtInference": performance.get("tensorRtInference", {}),
            "modelProfile": performance.get("modelProfile", {}),
            "resources": resource_summary(row.get("resourceCsv"))}


def load_launch(path):
    launch = read_json(path)
    cold = [load_sample(launch, row) for row in launch.get("coldRuns", [])]
    block, resident, warm = launch.get("warm", {}), {}, []
    if block.get("summary") and Path(block["summary"]).is_file():
        resident = read_json(block["summary"])
        warm = [load_sample(launch, row) for row in resident.get("runs", [])]
    ids = [str(i) for i in block.get("workerProcessIds", [])]
    warm_valid = (block.get("status") == "PASS" and block.get("residentApplication") is True
                  and not block.get("newProcessPerRun") and not block.get("relabelledFreshProcess")
                  and resident.get("applicationResident") is True and resident.get("independentWorkerPerRun") is True
                  and resident.get("pass") is True and len(ids) == 4 and len(set(ids)) == 4
                  and len([r for r in resident.get("rows", []) if r.get("excludedWarmup") is True]) == 1
                  and len(warm) == 3 and all(r["pass"] for r in warm))
    cold_valid = (len(cold) == 3 and all(r["pass"] for r in cold)
                  and all(r.get("newProcessPerRun") is True and not r.get("residentApplication")
                          for r in launch.get("coldRuns", [])))
    return {"path": str(Path(path).resolve()), "sourceHead": launch.get("sourceHead"),
            "coldValid": cold_valid, "warmValid": warm_valid, "cold": cold, "warm": warm,
            "residentWorkerIds": ids, "residentParentId": resident.get("processId"), "screen": resident.get("screen"),
            "residentResources": resource_summary(block.get("resourceCsv")),
            "resourceSamplingBoundary": launch.get("resourceSamplingBoundary"),
            "osCacheState": launch.get("osCacheState"), "modelInitializationState": launch.get("modelInitializationState")}


def summarize_rows(rows):
    result = {key: stats(row.get(key) for row in rows if row["pass"]) for key in
              ("elapsedMs", "clickToFirstInferenceMs", "clickToFormalMs")}
    for group in ("pipelineStages", "sourceStages", "timelineFromClickMs", "tensorRtInference"):
        keys = sorted({k for r in rows if r["pass"] for k in r[group]})
        result[group] = {k: stats(r[group].get(k) for r in rows if r["pass"])
                         for k in keys if k != "cudaEventIntervals"}
    return result


def compare(base, new, mode):
    valid = base[mode + "Valid"] and new[mode + "Valid"]
    b, n = base[mode], new[mode]
    signatures = [r["signature"] for r in b + n]
    mismatch = sorted({key for s in signatures[1:] for key in signatures[0]
                       if s.get(key) != signatures[0][key]}) if signatures else []
    comparable = valid and not mismatch
    bs, ns = summarize_rows(b), summarize_rows(n)
    ratios = {}
    for metric in ("elapsedMs", "clickToFirstInferenceMs", "clickToFormalMs"):
        denominator = ns[metric]["mean"]
        ratios[metric] = (bs[metric]["mean"] / denominator if comparable and denominator
                          and bs[metric]["mean"] is not None else None)
    return {"available": comparable, "ratio": ratios["elapsedMs"],
            "reason": "three passed comparable samples per side; baseline mean/new mean" if comparable else
                      "missing/failed evidence or different inputs/configuration; no ratio calculated",
            "signatureDifferences": mismatch, "baseline": bs, "new": ns, "speedup": ratios}


def summarize(args):
    output = output_path(args.output)
    launches = [load_launch(path) for path in args.benchmark]
    baseline = load_launch(args.baseline) if args.baseline else None
    comparisons = [{"new": launch["path"], "cold": compare(baseline, launch, "cold"),
                    "warm": compare(baseline, launch, "warm")} for launch in launches] if baseline else []
    matrices = []
    for path in args.matrix:
        matrix = read_json(path)
        gates = [r for r in matrix.get("rows", []) if not r.get("parent") and
                 (r.get("id", "").startswith("R-") or r.get("id", "").startswith("CTest-"))]
        matrices.append({"path": str(Path(path).resolve()), "nativeR19Pass": matrix.get("nativeR19Pass"),
                         "gates": [{k: r.get(k) for k in ("id", "status", "detail")} for r in gates],
                         "allRecordedGatesPass": bool(gates) and all(r["status"] == "PASS" for r in gates)})
    summary = {"schema": "signal-studio-ss-de-rpo-summary-v2", "percentileMethod": "linear interpolation; no zero fill",
               "matrices": matrices, "baseline": baseline, "launches": launches, "comparisons": comparisons,
               "boundaries": {
                   "cold": "new QApplication per trial; click after source/chart settle; OS cache not flushed",
                   "resident": "one QApplication/MainWindow; excluded warmup; fresh Worker/model/context every run",
                   "stages": "parent monotonic observations; host totals can overlap/nest; absent stages stay absent",
                   "resources": "sampled root+direct Workers; CPU includes process life/load; whole GPU including display; volume includes other writers",
                   "quality": "recorded IQ is separate from synthetic Golden, fault injection and field RF accuracy"}}
    output.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"output": str(output), "comparisons": [
        {"cold": row["cold"]["available"], "warm": row["warm"]["available"]} for row in comparisons]}))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--benchmark", action="append", default=[])
    parser.add_argument("--matrix", action="append", default=[])
    parser.add_argument("--baseline")
    parser.add_argument("--output", required=True)
    summarize(parser.parse_args())


if __name__ == "__main__":
    main()
