import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import resource
import shutil
import signal
import subprocess
import sys
import time


MIB = 1024 * 1024


def setting(name, default):
    value = os.environ.get(name)
    if value is None:
        return default
    if not value.isascii() or not value.isdecimal() or int(value) <= 0:
        raise ValueError(name + " must be a positive decimal MiB value")
    return int(value)


def available_memory():
    for line in Path("/proc/meminfo").read_text().splitlines():
        if line.startswith("MemAvailable:"):
            return int(line.split()[1]) * 1024
    raise RuntimeError("MemAvailable is unavailable")


def group_path(unit):
    result = subprocess.run(["systemctl", "--user", "show", unit, "--property=ControlGroup", "--value"],
                            capture_output=True, text=True, timeout=3)
    if result.returncode or not result.stdout.strip():
        return None
    path = Path("/sys/fs/cgroup") / result.stdout.strip().lstrip("/")
    return path if (path / "memory.max").exists() else None


def memory_sample(path):
    sample = {}
    if path is None:
        return sample
    for name in ("memory.current", "memory.peak", "memory.max", "memory.swap.current", "memory.swap.max"):
        try:
            sample[name] = int((path / name).read_text().strip())
        except FileNotFoundError:
            pass
    for name in ("memory.events", "memory.stat"):
        try:
            values = dict(line.split() for line in (path / name).read_text().splitlines())
            if name == "memory.stat":
                values = {key: value for key, value in values.items() if key in ("anon", "file", "shmem", "kernel", "pagetables")}
            sample[name] = {key: int(value) for key, value in values.items()}
        except FileNotFoundError:
            pass
    try:
        sample["pids"] = [int(pid) for pid in (path / "cgroup.procs").read_text().split()]
    except FileNotFoundError:
        pass
    return sample


def kill_group(unit, sig, path=None):
    if sig == "SIGKILL" and path is not None:
        try:
            (path / "cgroup.kill").write_text("1")
            return True
        except OSError:
            pass
    result = subprocess.run(["systemctl", "--user", "kill", "--kill-whom=all", "--signal=" + sig, unit],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3)
    return result.returncode == 0


def main():
    parser = argparse.ArgumentParser(description="Run a game in an isolated Linux memory cgroup and log its memory use")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command
    if command[:1] == ["--"]:
        command = command[1:]
    if not command:
        parser.error("a command is required")
    process = None
    unit = "anyps5-game-" + str(os.getpid()) + "-" + str(time.monotonic_ns()) + ".scope"
    interrupted = False
    reason = None
    path = None
    peak = 0

    def interrupt(signum, frame):
        nonlocal interrupted
        interrupted = True

    try:
        if sys.platform != "linux" or not Path("/sys/fs/cgroup/cgroup.controllers").exists():
            raise RuntimeError("The memory guard requires Linux cgroup v2")
        if not shutil.which("systemd-run") or not shutil.which("systemctl"):
            raise RuntimeError("The memory guard requires a systemd user manager")
        reserve = setting("ANYPS5_MEMORY_RESERVE_MIB", 6144) * MIB
        available = available_memory()
        budget = min(setting("ANYPS5_MEMORY_MAX_MIB", 18432) * MIB, available - reserve)
        budget = (budget // MIB) * MIB
        if budget < 32 * MIB:
            raise RuntimeError("Insufficient available RAM for a guarded run while retaining the requested reserve")
        stop_below = min(reserve, 4096 * MIB)
        default_log = Path(__file__).resolve().parents[1] / "build/profiles/memory-guard" / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ") + ".jsonl")
        log_path = Path(os.environ.get("ANYPS5_MEMORY_LOG", str(default_log)))
        log_path.parent.mkdir(parents=True, exist_ok=True)
        environment = dict(os.environ, ANYPS5_MEMORY_GUARD_ACTIVE=unit)
        limit = resource.getrlimit(resource.RLIMIT_CORE)
        resource.setrlimit(resource.RLIMIT_CORE, (0, limit[1]))
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(sig, interrupt)
        print("[memory-guard] unit=" + unit + " max=" + str(budget // MIB) + " MiB reserve=" + str(reserve // MIB) + " MiB log=" + str(log_path), flush=True)
        with log_path.open("x", buffering=1) as log:
            def record(kind, **values):
                log.write(json.dumps(dict(kind=kind, utc=datetime.now(timezone.utc).isoformat(), monotonic_ns=time.monotonic_ns(), **values)) + "\n")

            record("start", unit=unit, command=command, memory_max_bytes=budget, reserve_bytes=reserve, stop_available_bytes=stop_below)
            process = subprocess.Popen(["systemd-run", "--user", "--scope", "--quiet", "--expand-environment=no", "--unit=" + unit,
                                        "--property=MemoryMax=" + str(budget), "--property=MemorySwapMax=0", "--property=OOMPolicy=kill", *command], env=environment)
            deadline = time.monotonic() + 10
            next_sample = 0.0
            while process.poll() is None:
                now = time.monotonic()
                if path is None:
                    path = group_path(unit)
                if path is None and now >= deadline:
                    reason = "The memory cgroup could not be verified"
                if path is not None:
                    try:
                        if int((path / "memory.max").read_text().strip()) != budget:
                            reason = "The requested memory limit is not active"
                    except FileNotFoundError:
                        if process.poll() is None:
                            reason = "The memory cgroup disappeared while the game was running"
                available = available_memory()
                if available < stop_below:
                    reason = "Available system RAM fell below the safety reserve"
                if now >= next_sample:
                    sample = memory_sample(path)
                    peak = max(peak, sample.get("memory.current", 0), sample.get("memory.peak", 0))
                    record("sample", available_bytes=available, **sample)
                    next_sample = now + 1
                if interrupted or reason is not None:
                    if not kill_group(unit, "SIGKILL" if reason else "SIGTERM", path):
                        process.kill()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        kill_group(unit, "SIGKILL", path)
                        process.wait(timeout=5)
                    break
                time.sleep(0.25)
            code = process.wait()
            final_sample = memory_sample(path)
            peak = max(peak, final_sample.get("memory.current", 0), final_sample.get("memory.peak", 0))
            record("exit", returncode=code, reason=reason, interrupted=interrupted, observed_peak_bytes=peak,
                   final_cgroup_counters_available="memory.peak" in final_sample, **final_sample)
        if reason:
            print("[memory-guard] Stopped the game: " + reason, file=sys.stderr)
            return 137
        if interrupted:
            return 130
        if code:
            print("[memory-guard] Game exited with status " + str(code) + "; memory measurements: " + str(log_path), file=sys.stderr)
        return code if code >= 0 else 128 - code
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print("[memory-guard] " + str(error), file=sys.stderr)
        return 1
    finally:
        if process is not None:
            try:
                stopped = kill_group(unit, "SIGKILL", path)
                if process.poll() is None:
                    if not stopped:
                        process.kill()
                    process.wait(timeout=5)
            except (OSError, subprocess.SubprocessError):
                if process.poll() is None:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    sys.exit(main())
