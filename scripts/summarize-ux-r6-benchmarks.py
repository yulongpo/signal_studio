"""Summarize actual run/worker/CUDA measurements; never fills missing data with 0."""
import argparse
import csv
import datetime as dt
import json
from pathlib import Path
import statistics


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def stats(values):
    values = sorted(float(x) for x in values)
    if not values:
        return {"available": False, "n": 0}
    def quantile(q):
        p = q * (len(values)-1)
        low = int(p)
        return values[low] + (values[min(low+1, len(values)-1)]-values[low])*(p-low)
    return dict(available=True, n=len(values), minimum=values[0], maximum=values[-1],
                mean=statistics.mean(values), p50=quantile(.5), p95=quantile(.95))


def gpu_samples(launch, measurement):
    path = Path(launch["gpuCsv"])
    if not path.is_file():
        return dict(available=False)
    start = dt.datetime.fromisoformat(measurement["startedUtc"].replace("Z", "+00:00"))
    end = start + dt.timedelta(milliseconds=measurement["elapsedMs"])
    zone = dt.timezone(dt.timedelta(minutes=launch["gpuLocalUtcOffsetMinutes"]))
    rows = []
    with path.open(encoding="utf-8-sig", errors="replace") as stream:
        for raw in csv.DictReader(stream):
            row = {k.strip(): v.strip() for k, v in raw.items() if k}
            try:
                timestamp = dt.datetime.strptime(row["timestamp"], "%Y/%m/%d %H:%M:%S.%f").replace(tzinfo=zone)
                if start <= timestamp <= end and row.get("name") == measurement["run"].get("device"):
                    rows.append(row)
            except (ValueError, KeyError):
                continue
    metrics = {}
    for key in rows[0] if rows else []:
        values = []
        for row in rows:
            try:
                values.append(float(row[key].split(" ")[0]))
            except (ValueError, KeyError):
                pass
        if values:
            metrics[key] = stats(values)
    return dict(available=bool(rows), samples=len(rows), metrics=metrics,
                attribution="whole selected GPU sampled at 200ms; not exclusive worker allocation")


def summarize(root):
    root = Path(root)
    launcher = read(root / "launcher.json")
    rows = []
    for launch in launcher["runs"]:
        m = read(launch["measurement"])
        run = m.get("run", {})
        profile = run.get("performance", {})
        worker = profile.get("worker", {})
        row = dict(launchPass=launch["pass"], lifecycle=run.get("lifecycle", "Rejected"),
                   report=launch["measurement"], elapsedMs=m.get("elapsedMs"),
                   eligibleWindows=run.get("eligibleWindows"), completedWindows=run.get("completedWindows"),
                   windowsPerSecond=m.get("windowsPerSecond"), error=run.get("error", m.get("error")),
                   firstDecodedCandidateMs=profile.get("firstDecodedCandidateMs"),
                   frequencyTracksReadyMs=profile.get("frequencyTracksReadyMs"),
                   worker=worker, modelGpu=profile.get("gpu"), pipelineStages=profile.get("pipelineStages"),
                   sourceStages=profile.get("sourceStages"), logicalBatchDistribution=profile.get("logicalBatchDistribution"),
                   mainMemory=m.get("memoryAfter"), quality=m.get("quality"),
                   gpu=gpu_samples(launch, m) if run else dict(available=False))
        before=m.get("memoryBefore",{}); after=m.get("memoryAfter",{})
        if "cpuMilliseconds" in before and "cpuMilliseconds" in after:
            row["mainCpuMilliseconds"]=after["cpuMilliseconds"]-before["cpuMilliseconds"]
        cpu=worker.get("resource",{}).get("cpuMilliseconds")
        if cpu is not None and "mainCpuMilliseconds" in row and m.get("elapsedMs",0)>0:
            row["combinedCpuPercentOfOneLogicalCore"]=100*(cpu+row["mainCpuMilliseconds"])/m["elapsedMs"]
        rows.append(row)
    completed=[r for r in rows if r["lifecycle"]=="Completed" and r["launchPass"]]
    aggregates={key:stats(r[key] for r in completed if isinstance(r.get(key),(int,float)) and r[key]>=0)
                for key in ["elapsedMs","windowsPerSecond","firstDecodedCandidateMs","frequencyTracksReadyMs","combinedCpuPercentOfOneLogicalCore"]}
    result=dict(schema="signal-studio-ux-r6-benchmark-summary-v1", launcher=str(root/"launcher.json"),
                runs=rows, completedRunStatistics=aggregates,
                percentileMethod="linear interpolation over actual repeated complete runs; report n; no missing-data substitution",
                boundary="decoded candidate/frequency readiness are headless, not native UI paint or new streaming stability; host/source/GPU timings nest")
    (root/"summary.json").write_text(json.dumps(result, ensure_ascii=False, indent=2),encoding="utf-8")
    print(json.dumps({"output":str(root/"summary.json"),"completed":len(completed),"statistics":aggregates},ensure_ascii=False))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory")
    summarize(parser.parse_args().directory)
