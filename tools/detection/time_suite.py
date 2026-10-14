"""Run the frozen M4 paired matrix sequentially and preserve failed history."""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from time_evaluation import (
    read_json,
    require_execution_freeze,
    sha256,
    truth_events,
)

sys.dont_write_bytecode = True
REPOSITORY = Path(__file__).resolve().parents[2]
CHECKPOINT_SHA = (
    "acd997957dd85b7fc560897874b575d4776e80270d7e377d1de7559e513e40e0"
)
SCOPE_MATRIX = {
    "G-sample": ("G", "sample", False),
    "G-roi": ("G", "roi", False),
    "G-coarse": ("G", "coarse", False),
    "G-no-split": ("G", "no-split", False),
    "G-roi-no-split": ("G", "roi-no-split", False),
    "G-coarse-no-split": ("G", "coarse-no-split", False),
    "T-default": ("T", "sample", False),
    "T-sample-short": ("T-short", "sample", True),
    "T-roi-short": ("T-short", "roi", True),
}
NATIVE_WORK_BYTES = 256 * 1024 * 1024
NATIVE_EVIDENCE_BYTES = 128 * 1024 * 1024
REFERENCE_WORK_BYTES = 2048 * 1024 * 1024


def utc_now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".writing")
    temporary.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def freeze_entries(frozen: dict) -> dict[Path, str]:
    entries = frozen.get("files", [])
    if isinstance(entries, dict):
        return {
            Path(path).resolve(): digest for path, digest in entries.items()
        }
    return {Path(item["path"]).resolve(): item["sha256"] for item in entries}


def require_frozen_file(path: Path, entries: dict) -> str:
    resolved = path.resolve()
    expected = entries.get(resolved)
    if expected is None or sha256(resolved) != expected:
        raise ValueError(f"Required suite identity is not frozen: {resolved}")
    return expected


def verify_asset(asset: dict) -> dict:
    path = Path(asset["path"]).resolve()
    if sha256(path) != asset["sha256"]:
        raise ValueError(f"Frozen dataset asset changed: {path}")
    return {"path": str(path), "sha256": asset["sha256"]}


def load_datasets(gui_manifest: Path, holdout_manifest: Path) -> dict:
    gui = read_json(gui_manifest)
    if (
        gui.get("schema") != "signalstudio-m4-validation-input-audit-v1"
        or gui.get("baselineId") != "gui-local10-m0-20261013"
        or len(gui.get("guiFiles", [])) != 10
    ):
        raise ValueError("G requires the complete frozen local10 input set")
    g_samples = []
    for item in gui["guiFiles"]:
        header = item["wavHeader"]
        if (
            header["sampleRateHz"] != 20000000
            or header["sampleCount"] != 16000000
            or header["durationSeconds"] != 0.8
        ):
            raise ValueError("G must contain complete 0.8 s / 20 Msps files")
        g_samples.append(
            {
                "sampleId": item["sampleId"],
                "group": "G",
                "source": verify_asset(item["wav"]),
                "truth": verify_asset(item["txtLabel"]),
                "sampleRateHz": header["sampleRateHz"],
                "physicalSamples": str(header["sampleCount"]),
                "centerFrequencyHz": 2.5e9,
                "effectiveBandwidthHz": 20000000,
                "expectedTruthEvents": item["timePairCount"],
            }
        )
    if sum(s["expectedTruthEvents"] for s in g_samples) != 594:
        raise ValueError("G frozen truth count must remain 594")
    holdout = read_json(holdout_manifest)
    if (
        holdout.get("schema") != "signalstudio-m4-independent-holdout-v1"
        or holdout.get("complete") is not True
        or holdout.get("fileCount") != 96
        or len(holdout.get("samples", [])) != 96
        or holdout.get("synthetic") is not True
        or holdout.get("realRfCapture") is not False
    ):
        raise ValueError("T requires the complete 96-file synthetic manifest")
    t_samples = []
    for item in holdout["samples"]:
        sample = {
            "sampleId": item["sampleId"],
            "group": item["group"],
            "source": verify_asset(item["wav"]),
            "truth": verify_asset(item["truth"]),
            "sampleRateHz": item["sampleRateHz"],
            "physicalSamples": item["physicalSamples"],
            "centerFrequencyHz": item["centerFrequencyHz"],
            "effectiveBandwidthHz": item["sampleRateHz"],
            "expectedTruthEvents": item["truthEvents"],
        }
        exact = read_json(Path(sample["truth"]["path"]))
        if (
            exact.get("schema") != "signalstudio-m4-sample-exact-truth-v1"
            or exact.get("truthAuthority") != "sample-exact"
            or exact.get("sampleId") != sample["sampleId"]
        ):
            raise ValueError(
                "T evaluator truth must be private sample-exact JSON"
            )
        verify_asset(item["txtLabel"])
        t_samples.append(sample)
    datasets = {
        "G": sorted(g_samples, key=lambda s: s["sampleId"]),
        "T": sorted(t_samples, key=lambda s: s["sampleId"]),
        "T-short": sorted(
            [s for s in t_samples if s["group"] == "short-1-to-8ms"],
            key=lambda s: s["sampleId"],
        ),
    }
    if len(datasets["T-short"]) != 32:
        raise ValueError("Short holdout requires all 32 selected files")
    for samples in datasets.values():
        ids = [sample["sampleId"] for sample in samples]
        if len(ids) != len(set(ids)):
            raise ValueError("Duplicate sample ID in the frozen dataset")
    return datasets


def build_scope_plan(datasets: dict, scopes: list[str], maximum=None) -> list:
    plans = []
    for scope in scopes:
        dataset, strategy, short = SCOPE_MATRIX[scope]
        all_samples = datasets[dataset]
        selected = all_samples if maximum is None else all_samples[:maximum]
        plans.append(
            {
                "scope": scope,
                "strategy": strategy,
                "shortProfile": short,
                "semantics": "product-v2",
                "plannedFileCount": len(all_samples),
                "selectedFileCount": len(selected),
                "diagnosticPartial": maximum is not None,
                "samples": selected,
                "expectedTruthCount": sum(
                    s["expectedTruthEvents"] for s in selected
                ),
            }
        )
    return plans


class _BasicLimits(ctypes.Structure):
    _fields_ = [
        ("process_time", ctypes.c_longlong),
        ("job_time", ctypes.c_longlong),
        ("flags", wintypes.DWORD),
        ("minimum_ws", ctypes.c_size_t),
        ("maximum_ws", ctypes.c_size_t),
        ("active_process_limit", wintypes.DWORD),
        ("affinity", ctypes.c_size_t),
        ("priority", wintypes.DWORD),
        ("scheduling", wintypes.DWORD),
    ]


class _IoCounters(ctypes.Structure):
    _fields_ = [
        (name, ctypes.c_ulonglong)
        for name in (
            "read_ops",
            "write_ops",
            "other_ops",
            "read_bytes",
            "write_bytes",
            "other_bytes",
        )
    ]


class _ExtendedLimits(ctypes.Structure):
    _fields_ = [
        ("basic", _BasicLimits),
        ("io", _IoCounters),
        ("process_memory", ctypes.c_size_t),
        ("job_memory", ctypes.c_size_t),
        ("peak_process_memory", ctypes.c_size_t),
        ("peak_job_memory", ctypes.c_size_t),
    ]


class _MemoryCounters(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("page_faults", wintypes.DWORD),
        ("peak_ws", ctypes.c_size_t),
        ("working_set", ctypes.c_size_t),
        ("peak_paged", ctypes.c_size_t),
        ("paged", ctypes.c_size_t),
        ("peak_nonpaged", ctypes.c_size_t),
        ("nonpaged", ctypes.c_size_t),
        ("pagefile", ctypes.c_size_t),
        ("peak_pagefile", ctypes.c_size_t),
    ]


class WindowsJob:
    """Closing this job terminates only the launched stage and its children."""

    def __init__(self, pid: int):
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel.CreateJobObjectW.argtypes = [
            ctypes.c_void_p,
            wintypes.LPCWSTR,
        ]
        self.kernel.CreateJobObjectW.restype = wintypes.HANDLE
        self.kernel.OpenProcess.argtypes = [
            wintypes.DWORD,
            wintypes.BOOL,
            wintypes.DWORD,
        ]
        self.kernel.OpenProcess.restype = wintypes.HANDLE
        self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel.SetInformationJobObject.argtypes = [
            wintypes.HANDLE,
            ctypes.c_int,
            ctypes.c_void_p,
            wintypes.DWORD,
        ]
        self.kernel.AssignProcessToJobObject.argtypes = [
            wintypes.HANDLE,
            wintypes.HANDLE,
        ]
        self.handle = self.kernel.CreateJobObjectW(None, None)
        if not self.handle:
            raise OSError(ctypes.get_last_error(), "CreateJobObjectW failed")
        limits = _ExtendedLimits()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.kernel.SetInformationJobObject(
            self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)
        ):
            self.close()
            raise OSError(ctypes.get_last_error(), "Job kill-on-close failed")
        process_handle = self.kernel.OpenProcess(0x0101, False, pid)
        try:
            if not process_handle or not self.kernel.AssignProcessToJobObject(
                self.handle, process_handle
            ):
                error = ctypes.get_last_error()
                self.close()
                raise OSError(error, "Cannot contain stage process tree")
        finally:
            if process_handle:
                self.kernel.CloseHandle(process_handle)

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def main_process_rss(pid: int):
    if os.name != "nt":
        return None
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [
        wintypes.DWORD,
        wintypes.BOOL,
        wintypes.DWORD,
    ]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.OpenProcess(0x0410, False, pid)
    if not handle:
        return None
    try:
        counters = _MemoryCounters()
        counters.cb = ctypes.sizeof(counters)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        psapi.GetProcessMemoryInfo.argtypes = [
            wintypes.HANDLE,
            ctypes.c_void_p,
            wintypes.DWORD,
        ]
        if psapi.GetProcessMemoryInfo(
            handle, ctypes.byref(counters), counters.cb
        ):
            return int(counters.working_set)
    finally:
        kernel.CloseHandle(handle)
    return None


def gpu_resource_sample():
    executable = shutil.which("nvidia-smi")
    if not executable:
        return None
    try:
        query = subprocess.run(
            [
                executable,
                "--query-gpu=index,memory.used,utilization.gpu",
                "--format=csv,noheader,nounits",
            ],
            capture_output=True,
            text=True,
            timeout=4,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        )
        if query.returncode:
            return None
        return [
            {
                "gpuIndex": int(values[0]),
                "wholeDeviceMemoryUsedMiB": int(values[1]),
                "wholeDeviceUtilizationPercent": int(values[2]),
            }
            for line in query.stdout.splitlines()
            if len(values := [v.strip() for v in line.split(",")]) == 3
        ]
    except (OSError, subprocess.SubprocessError, ValueError):
        return None


def terminate_stage(process: subprocess.Popen, job):
    if job is not None:
        job.close()
    elif os.name == "nt":
        subprocess.run(
            ["taskkill", "/PID", str(process.pid), "/T", "/F"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            creationflags=subprocess.CREATE_NO_WINDOW,
            check=False,
        )
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    try:
        process.wait(timeout=15)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=15)


def validate_result(path: Path, identity: dict, stage: str) -> dict:
    document = read_json(path)
    sample = identity["sample"]
    if (
        Path(document["sourcePath"]).resolve()
        != Path(sample["source"]["path"]).resolve()
        or document.get("sampleRateHz") != sample["sampleRateHz"]
        or document.get("centerFrequencyHz") != sample["centerFrequencyHz"]
        or document.get("physicalSamples") != sample["physicalSamples"]
        or document.get("strategy") != identity["strategy"]
        or document.get("shortProfile") != identity["shortProfile"]
        or not isinstance(document.get("pass"), bool)
    ):
        raise ValueError("Existing result source or configuration mismatch")
    if stage == "reference":
        if (
            document.get("schema")
            != "signalstudio-m4-independent-time-reference-v1"
            or document.get("sourceSha256") != sample["source"]["sha256"]
            or document.get("executionFreezeSha256")
            != identity["executionFreezeSha256"]
            or document.get("checkpointSha256") != CHECKPOINT_SHA
            or document.get("semantics") != "product-v2"
            or document.get("referenceWorkingBudgetBytes")
            != REFERENCE_WORK_BYTES
        ):
            raise ValueError(
                "Existing reference does not match frozen semantics"
            )
    else:
        run = document.get("run", {})
        request = run.get("request", {})
        if (
            document.get("schemaVersion") != 1
            or run.get("schema") != "signal-studio-detection-run"
            or run.get("version") not in (1, 2)
            or request.get("schemaVersion") != 1
            or request.get("profile", {}).get("id") != "ffscn-gui-iq-v1"
            or request.get("workingSetBudgetBytes") != str(NATIVE_WORK_BYTES)
            or request.get("evidenceBudgetBytes") != str(NATIVE_EVIDENCE_BYTES)
        ):
            raise ValueError("Existing native result schema or budget mismatch")
    return document


def validate_resume(directory: Path, identity: dict, stage: str):
    launch_path = directory / "launch.json"
    completion_path = directory / "completion.json"
    if not launch_path.exists():
        if directory.exists() and any(directory.iterdir()):
            raise ValueError("Unowned artifacts cannot be adopted for resume")
        return None
    launch = read_json(launch_path)
    if launch.get("identity") != identity or launch.get("stage") != stage:
        raise ValueError(
            "Resume identity changed; choose a new experiment root"
        )
    result_path = directory / "result.json"
    if completion_path.exists():
        completion = read_json(completion_path)
        expected_hash = completion.get("resultSha256")
        if expected_hash is not None:
            if not result_path.exists() or sha256(result_path) != expected_hash:
                raise ValueError("Completed result was replaced or removed")
            if not completion.get("failed"):
                validate_result(result_path, identity, stage)
        elif result_path.exists():
            raise ValueError(
                "A result appeared after a recorded missing-result failure"
            )
        return {**completion, "resumed": True}
    process_path = directory / "process-identity.json"
    if process_path.exists() and pid_active(read_json(process_path)["pid"]):
        raise ValueError(
            "An unfinished stage PID is still active; no GPU stage may follow it"
        )
    # Never automatically retry an interrupted file after observing its outcome.
    return {
        "schema": "signalstudio-m4-stage-completion-v1",
        "failed": True,
        "reason": "interrupted-existing-stage-without-completion",
        "resultSha256": None,
        "resultPath": str(result_path.resolve()),
        "resumed": True,
        "incompleteHistoryPreserved": True,
    }


def pid_active(pid: int) -> bool:
    if os.name != "nt":
        try:
            os.kill(pid, 0)
            return True
        except ProcessLookupError:
            return False
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [
        wintypes.DWORD,
        wintypes.BOOL,
        wintypes.DWORD,
    ]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.GetExitCodeProcess.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(wintypes.DWORD),
    ]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        return False
    try:
        status = wintypes.DWORD()
        return (
            bool(kernel.GetExitCodeProcess(handle, ctypes.byref(status)))
            and status.value == 259
        )
    finally:
        kernel.CloseHandle(handle)


def run_stage(
    command: list[str],
    directory: Path,
    identity: dict,
    stage: str,
    timeout: float,
    resources=True,
):
    resumed = validate_resume(directory, identity, stage)
    if resumed is not None:
        return resumed
    directory.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    launch_path = directory / "launch.json"
    launch = {
        "schema": "signalstudio-m4-stage-launch-v1",
        "stage": stage,
        "identity": identity,
        "command": command,
        "startedAtUtc": utc_now(),
        "timeoutSeconds": timeout,
    }
    write_json(launch_path, launch)
    process = None
    job = None
    reason = ""
    timed_out = False
    return_code = None
    rss_peak = None
    gpu_peak = {}
    try:
        with (directory / "process.log").open("wb") as log:
            process = subprocess.Popen(
                command,
                cwd=REPOSITORY,
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
                creationflags=(
                    subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
                ),
                start_new_session=os.name != "nt",
            )
            if os.name == "nt":
                job = WindowsJob(process.pid)
            write_json(
                directory / "process-identity.json",
                {"pid": process.pid, "stage": stage},
            )
            with (directory / "resources.jsonl").open(
                "w", encoding="utf-8"
            ) as samples:
                while process.poll() is None:
                    elapsed = time.monotonic() - started
                    if elapsed > timeout:
                        timed_out = True
                        reason = "subprocess-timeout"
                        terminate_stage(process, job)
                        break
                    if resources:
                        rss = main_process_rss(process.pid)
                        gpu = gpu_resource_sample()
                        if rss is not None:
                            rss_peak = max(rss_peak or 0, rss)
                        for value in gpu or []:
                            index = str(value["gpuIndex"])
                            gpu_peak[index] = max(
                                gpu_peak.get(index, 0),
                                value["wholeDeviceMemoryUsedMiB"],
                            )
                        samples.write(
                            json.dumps(
                                {
                                    "elapsedSeconds": elapsed,
                                    "mainProcessRssBytes": rss,
                                    "wholeDeviceGpu": gpu,
                                }
                            )
                            + "\n"
                        )
                        samples.flush()
                    try:
                        process.wait(
                            timeout=min(5, max(0.01, timeout - elapsed))
                        )
                    except subprocess.TimeoutExpired:
                        pass
                return_code = process.wait(timeout=15)
    except KeyboardInterrupt:
        reason = "suite-interrupted"
        if process is not None:
            terminate_stage(process, job)
        raise
    except (OSError, subprocess.SubprocessError) as error:
        reason = f"subprocess-error: {error}"
        if process is not None:
            terminate_stage(process, job)
    finally:
        if job is not None:
            job.close()
    result_path = directory / "result.json"
    result_hash = sha256(result_path) if result_path.exists() else None
    pass_value = False
    if result_hash is not None:
        try:
            result = validate_result(result_path, identity, stage)
            pass_value = result["pass"]
            if not pass_value and not reason:
                reason = "result-not-complete"
        except (KeyError, TypeError, ValueError) as error:
            reason = f"invalid-result: {error}"
    elif not reason:
        reason = "missing-result"
    failed = bool(reason) or return_code != 0 or not pass_value
    completion = {
        "schema": "signalstudio-m4-stage-completion-v1",
        "stage": stage,
        "finishedAtUtc": utc_now(),
        "returnCode": return_code,
        "timedOut": timed_out,
        "failed": failed,
        "reason": reason,
        "resultPath": str(result_path.resolve()),
        "resultSha256": result_hash,
        "wallSeconds": time.monotonic() - started,
        "mainProcessPeakRssBytes": rss_peak,
        "wholeDeviceGpuPeakMemoryUsedMiB": gpu_peak,
        "resourceScope": "RSS is main PID; GPU memory is whole device, not attributable allocation",
        "processTreeContainment": (
            "Windows kill-on-close Job"
            if os.name == "nt"
            else "owned POSIX session"
        ),
        "resumed": False,
    }
    write_json(directory / "completion.json", completion)
    return completion


def pair_identity(sample, plan, context):
    return {
        "sample": sample,
        "strategy": plan["strategy"],
        "shortProfile": plan["shortProfile"],
        "semantics": "product-v2",
        "executionFreezeSha256": context["freezeSha256"],
        "nativeBinarySha256": context["nativeSha256"],
        "referenceSourceSha256": context["referenceSha256"],
        "configuration": context["configuration"],
        "nativeWorkingBudgetBytes": NATIVE_WORK_BYTES,
        "nativeEvidenceBudgetBytes": NATIVE_EVIDENCE_BYTES,
        "referenceWorkingBudgetBytes": REFERENCE_WORK_BYTES,
    }


def accounting_result(directory: Path, state: dict) -> Path:
    """Keep originals; a failed subprocess can never earn predictions."""
    original = directory / "result.json"
    if not state["failed"] or not original.exists():
        return original
    try:
        document = read_json(original)
    except (OSError, ValueError):
        return (
            original  # The evaluator already counts invalid/missing as failure.
        )
    if document.get("pass") is not True:
        return original
    overlay = directory / "failure-accounting-result.json"
    value = {
        **document,
        "pass": False,
        "error": state["reason"],
        "suiteFailureAccounting": {
            "originalResult": str(original.resolve()),
            "originalResultSha256": sha256(original),
            "reason": state["reason"],
            "subprocessReturnCode": state.get("returnCode"),
        },
    }
    if overlay.exists():
        if read_json(overlay) != value:
            raise ValueError("Existing failure accounting overlay changed")
    else:
        write_json(overlay, value)
    return overlay


def run_evaluator(command, scope_root, context, index_path):
    identity = {
        "executionFreezeSha256": context["freezeSha256"],
        "pairedIndexSha256": sha256(index_path),
        "evaluatorSha256": sha256(context["evaluator"]),
    }
    launch_path = scope_root / "evaluation-identity.json"
    completion_path = scope_root / "evaluation-completion.json"
    result_path = scope_root / "evaluation.json"
    if launch_path.exists():
        if read_json(launch_path) != identity:
            raise ValueError(
                "Evaluation identity changed; preserve the old scope"
            )
        if completion_path.exists():
            state = read_json(completion_path)
            digest = state.get("resultSha256")
            if digest is not None and (
                not result_path.exists() or sha256(result_path) != digest
            ):
                raise ValueError("Existing evaluation report changed")
            if digest is None and result_path.exists():
                raise ValueError(
                    "Late evaluation report changed failed history"
                )
            return state
        return {
            "returnCode": None,
            "reason": "interrupted-evaluation-history-preserved",
            "resultSha256": None,
        }
    write_json(launch_path, identity)
    started = time.monotonic()
    return_code = None
    reason = ""
    try:
        with (scope_root / "evaluation-process.log").open("wb") as log:
            evaluation = subprocess.run(
                command,
                cwd=REPOSITORY,
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=300,
                creationflags=(
                    subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
                ),
                check=False,
            )
            return_code = evaluation.returncode
    except (OSError, subprocess.SubprocessError) as error:
        reason = str(error)
    state = {
        "returnCode": return_code,
        "reason": reason,
        "wallSeconds": time.monotonic() - started,
        "resultSha256": sha256(result_path) if result_path.exists() else None,
    }
    write_json(completion_path, state)
    return state


def run_scope(plan, context, root, timeout, resources=True):
    scope_root = root / plan["scope"]
    scope_root.mkdir(parents=True, exist_ok=True)
    scope_identity = {
        "schema": "signalstudio-m4-suite-scope-v1",
        "plan": plan,
        "executionFreezeSha256": context["freezeSha256"],
        "configuration": context["configuration"],
    }
    scope_manifest = scope_root / "scope-manifest.json"
    if scope_manifest.exists():
        if read_json(scope_manifest) != scope_identity:
            raise ValueError(
                "Existing scope definition changed; use a new root"
            )
    else:
        write_json(scope_manifest, scope_identity)
    pairs = []
    index_samples = []
    for ordinal, sample in enumerate(plan["samples"], start=1):
        sample_root = scope_root / sample["sampleId"]
        native_dir = sample_root / "native"
        reference_dir = sample_root / "reference"
        identity = pair_identity(sample, plan, context)
        native_command = [
            str(context["native"]),
            "--file",
            sample["source"]["path"],
            str(native_dir),
            plan["strategy"],
        ]
        if plan["shortProfile"]:
            native_command.append("--short")
        print(
            f'{plan["scope"]} {ordinal}/{len(plan["samples"])} native {sample["sampleId"]}',
            flush=True,
        )
        native_state = run_stage(
            native_command, native_dir, identity, "native", timeout, resources
        )
        reference_command = [
            sys.executable,
            "-B",
            str(context["reference"]),
            "--file",
            sample["source"]["path"],
            "--output",
            str(reference_dir),
            "--strategy",
            plan["strategy"],
            "--semantics",
            "product-v2",
            "--checkpoint",
            str(context["checkpoint"]),
            "--device",
            "cuda:0",
            "--sample-budget-mib",
            "2048",
            "--center",
            str(sample["centerFrequencyHz"]),
            "--freeze",
            str(context["freeze"]),
        ]
        if plan["shortProfile"]:
            reference_command.append("--short")
        print(
            f'{plan["scope"]} {ordinal}/{len(plan["samples"])} reference {sample["sampleId"]}',
            flush=True,
        )
        reference_state = run_stage(
            reference_command,
            reference_dir,
            identity,
            "reference",
            timeout,
            resources,
        )
        pairs.append(
            {
                "sampleId": sample["sampleId"],
                "native": native_state,
                "reference": reference_state,
            }
        )
        native_accounting = accounting_result(native_dir, native_state)
        reference_accounting = accounting_result(reference_dir, reference_state)
        index_samples.append(
            {
                **{
                    k: v
                    for k, v in sample.items()
                    if k != "expectedTruthEvents"
                },
                "nativeResult": str(native_accounting.resolve()),
                "referenceResult": str(reference_accounting.resolve()),
                "nativeOriginalResult": str(
                    (native_dir / "result.json").resolve()
                ),
                "referenceOriginalResult": str(
                    (reference_dir / "result.json").resolve()
                ),
                "nativeRunFailure": native_state["failed"],
                "referenceRunFailure": reference_state["failed"],
            }
        )
        write_json(
            scope_root / "pair-run-records.json",
            {
                "scope": plan["scope"],
                "plannedFiles": plan["plannedFileCount"],
                "selectedFiles": plan["selectedFileCount"],
                "pairs": pairs,
            },
        )
    index = {
        "schema": "signalstudio-m4-paired-suite-index-v1",
        "parameters": {
            "strategy": plan["strategy"],
            "shortProfile": plan["shortProfile"],
            "semantics": "product-v2",
        },
        "scope": plan["scope"],
        "diagnosticPartial": plan["diagnosticPartial"],
        "plannedFileCount": plan["plannedFileCount"],
        "expectedTruthCount": plan["expectedTruthCount"],
        "samples": index_samples,
    }
    index_path = scope_root / "paired-index.json"
    if index_path.exists():
        if read_json(index_path) != index:
            raise ValueError(
                "Existing paired index changed; use a new scope root"
            )
    else:
        write_json(index_path, index)
    evaluation_path = scope_root / "evaluation.json"
    evaluation_command = [
        sys.executable,
        "-B",
        str(context["evaluator"]),
        "--index",
        str(index_path),
        "--freeze",
        str(context["freeze"]),
        "--output",
        str(evaluation_path),
    ]
    evaluation_state = run_evaluator(
        evaluation_command, scope_root, context, index_path
    )
    if evaluation_state["returnCode"] != 0:
        failed_summary = {
            "scope": plan["scope"],
            "assessment": "evaluation-failed",
            "acceptanceEligible": False,
            "evaluationReturnCode": evaluation_state["returnCode"],
            "reason": evaluation_state["reason"],
            "pairs": len(pairs),
            "diagnosticPartial": plan["diagnosticPartial"],
        }
        write_json(scope_root / "scope-summary.json", failed_summary)
        return failed_summary
    report = read_json(evaluation_path)
    summary = {
        "scope": plan["scope"],
        "plannedFileCount": plan["plannedFileCount"],
        "pairedFiles": len(pairs),
        "diagnosticPartial": plan["diagnosticPartial"],
        "acceptanceEligible": not plan["diagnosticPartial"]
        and len(pairs) == plan["plannedFileCount"],
        "assessment": (
            "diagnostic-partial-no-gate"
            if plan["diagnosticPartial"]
            else "complete-scope-numeric-results"
        ),
        "nativeFailedRuns": sum(p["native"]["failed"] for p in pairs),
        "referenceFailedRuns": sum(p["reference"]["failed"] for p in pairs),
        "evaluation": str(evaluation_path.resolve()),
        "evaluationSha256": sha256(evaluation_path),
        "pairedIndex": str(index_path.resolve()),
        "pairedIndexSha256": sha256(index_path),
        "includedSamples": [sample["sampleId"] for sample in plan["samples"]],
        "nativeAccounting": report["native"],
        "referenceAccounting": report["reference"],
        "pairedFileBootstrap": report["pairedFileBootstrap"],
        "numericResultsAreAcceptanceClaims": False,
        "referenceBaselineComplete": report["referenceBaselineComplete"],
    }
    write_json(scope_root / "scope-summary.json", summary)
    return summary


def run(args):
    frozen = require_execution_freeze(args.freeze, Path(__file__))
    entries = freeze_entries(frozen)
    bin_name = f"vs2026-qt611-{args.configuration.lower()}_bin"
    native = (
        REPOSITORY
        / "out"
        / bin_name
        / "SignalStudioDetectionTimePipelineTests.exe"
    )
    reference = REPOSITORY / "tools/detection/time_reference.py"
    evaluator = REPOSITORY / "tools/detection/time_evaluation.py"
    gui_manifest = (
        REPOSITORY
        / "docs/implementation/wideband-detection/M4/input-manifest.json"
    )
    holdout_manifest = (
        REPOSITORY
        / "out/tasks/wideband-detection/M4/assets/holdout/manifest.json"
    )
    for path in (
        native,
        reference,
        evaluator,
        gui_manifest,
        holdout_manifest,
        args.checkpoint,
    ):
        require_frozen_file(path, entries)
    if sha256(args.checkpoint) != CHECKPOINT_SHA:
        raise ValueError("Suite requires frozen epoch1200 checkpoint")
    datasets = load_datasets(gui_manifest, holdout_manifest)
    scopes = list(SCOPE_MATRIX) if args.scope == "all" else [args.scope]
    plans = build_scope_plan(datasets, scopes, args.max_files)
    context = {
        "freeze": args.freeze.resolve(),
        "freezeSha256": sha256(args.freeze),
        "native": native,
        "nativeSha256": sha256(native),
        "reference": reference,
        "referenceSha256": sha256(reference),
        "evaluator": evaluator,
        "checkpoint": args.checkpoint.resolve(),
        "configuration": args.configuration,
    }
    args.root = args.root.resolve()
    args.root.mkdir(parents=True, exist_ok=True)
    suite_identity = {
        "schema": "signalstudio-m4-suite-plan-v1",
        "executionFreezeSha256": context["freezeSha256"],
        "configuration": args.configuration,
        "scopeSelection": scopes,
        "maxFiles": args.max_files,
        "pairedRuns": sum(plan["selectedFileCount"] for plan in plans),
        "fullMatrixPairedRuns": 220,
        "diagnosticPartial": args.max_files is not None,
        "plans": plans,
        "sequentialNativeThenReference": True,
    }
    identity_path = args.root / "suite-plan.json"
    if identity_path.exists():
        if read_json(identity_path) != suite_identity:
            raise ValueError(
                "Existing suite identity differs; do not overwrite history"
            )
    else:
        write_json(identity_path, suite_identity)
    summaries = []
    for plan in plans:
        summaries.append(
            run_scope(
                plan,
                context,
                args.root,
                args.timeout_seconds,
                not args.no_resource_sampling,
            )
        )
        write_json(
            args.root / "suite-summary.json",
            {
                "schema": "signalstudio-m4-suite-summary-v1",
                "executionFreezeSha256": context["freezeSha256"],
                "scopes": summaries,
                "diagnosticPartial": args.max_files is not None,
                "allScopesDispatched": len(summaries) == len(plans),
                "formalAcceptanceEligible": args.max_files is None,
            },
        )
    return summaries


def self_check():
    g = [{"sampleId": f"g-{i}", "expectedTruthEvents": i} for i in range(10)]
    t = [{"sampleId": f"t-{i}", "expectedTruthEvents": 2} for i in range(96)]
    datasets = {"G": g, "T": t, "T-short": t[:32]}
    full = build_scope_plan(datasets, list(SCOPE_MATRIX))
    partial = build_scope_plan(datasets, list(SCOPE_MATRIX), 1)
    assert sum(p["selectedFileCount"] for p in full) == 220
    assert len(partial) == 9 and all(p["diagnosticPartial"] for p in partial)
    assert all(p["selectedFileCount"] == 1 for p in partial)
    assert SCOPE_MATRIX["G-roi-no-split"][1] == "roi-no-split"
    assert SCOPE_MATRIX["G-coarse-no-split"][1] == "coarse-no-split"
    diagnostics = REPOSITORY / "out/tasks/wideband-detection/M4/diagnostics"
    diagnostics.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="suite-cpu-", dir=diagnostics
    ) as temporary:
        root = Path(temporary).resolve()
        assert root.is_relative_to(diagnostics.resolve())
        identity = {
            "sample": {
                "source": {"path": str(root / "source.wav"), "sha256": "fixed"},
                "sampleRateHz": 20000000,
                "centerFrequencyHz": 2.5e9,
                "physicalSamples": "800000",
            },
            "strategy": "sample",
            "shortProfile": False,
            "executionFreezeSha256": "fixed",
        }
        directory = root / "native"
        write_json(
            directory / "launch.json", {"stage": "native", "identity": identity}
        )
        write_json(
            directory / "completion.json",
            {"failed": True, "resultSha256": None, "reason": "missing-result"},
        )
        resumed = validate_resume(directory, identity, "native")
        assert resumed["resumed"] and resumed["failed"]
        try:
            validate_resume(
                directory, {**identity, "shortProfile": True}, "native"
            )
            raise AssertionError("Changed parameters were accepted")
        except ValueError:
            pass
        (directory / "result.json").write_text("{}", encoding="utf-8")
        try:
            validate_resume(directory, identity, "native")
            raise AssertionError("Late result overwrite was accepted")
        except ValueError:
            pass
        another = root / "reference"
        write_json(
            another / "launch.json",
            {"stage": "reference", "identity": identity},
        )
        interrupted = validate_resume(another, identity, "reference")
        assert (
            interrupted["failed"] and interrupted["incompleteHistoryPreserved"]
        )
        (another / "result.json").write_text(
            json.dumps({"pass": True}), encoding="utf-8"
        )
        overlay = accounting_result(another, interrupted)
        assert read_json(overlay)["pass"] is False
        assert read_json(another / "result.json")["pass"] is True
        # Only a disposable Python sleeper runs: this proves containment and
        # timeout cleanup without starting Qt, a model or any GPU query.
        timed = run_stage(
            [sys.executable, "-B", "-c", "import time; time.sleep(3)"],
            root / "timeout",
            identity,
            "reference",
            0.5,
            False,
        )
        assert timed["timedOut"] and timed["failed"]
    return {
        "passed": True,
        "fullPairs": 220,
        "scopes": 9,
        "diagnosticOnePerScopePairs": 9,
        "checks": [
            "exact-scope-matrix",
            "partial-never-gate",
            "failed-resume-preserved",
            "changed-freeze-parameters-rejected",
            "late-result-rejected",
            "interrupted-history-kept",
            "failed-subprocess-accounting-overlay-preserves-original",
            "cpu-timeout-process-tree-contained",
        ],
        "formalRunsStarted": False,
        "gpuUsed": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--freeze", type=Path)
    parser.add_argument("--root", type=Path)
    parser.add_argument(
        "--configuration", choices=("Debug", "Release"), default="Debug"
    )
    parser.add_argument(
        "--scope", choices=("all", *SCOPE_MATRIX), default="all"
    )
    parser.add_argument(
        "--checkpoint",
        type=Path,
        default=Path(
            "D:/project/FFSCN_train2/FFSCN_TEST_GUI/config/epoch_1200.pth"
        ),
    )
    parser.add_argument("--max-files", type=int)
    parser.add_argument("--timeout-seconds", type=float, default=1500)
    parser.add_argument("--no-resource-sampling", action="store_true")
    parser.add_argument("--self-check", action="store_true")
    args = parser.parse_args()
    if args.self_check:
        print(json.dumps(self_check(), indent=2))
        return 0
    if not args.freeze or not args.root:
        parser.error("--freeze and --root are required")
    if args.max_files is not None and args.max_files < 1:
        parser.error("--max-files must be positive and is diagnostic only")
    if not 1 <= args.timeout_seconds <= 86400:
        parser.error("--timeout-seconds must be 1..86400")
    result = run(args)
    print(
        json.dumps(
            {
                "root": str(args.root),
                "scopes": len(result),
                "diagnosticPartial": args.max_files is not None,
            },
            indent=2,
        )
    )
    return (
        0 if all(s["assessment"] != "evaluation-failed" for s in result) else 1
    )


if __name__ == "__main__":
    raise SystemExit(main())
