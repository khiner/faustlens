#!/usr/bin/env python3
"""Measure forward AD in fresh processes and retain classified failures."""

import argparse
import hashlib
import json
import platform
import subprocess
from datetime import datetime, timezone
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path("build"))
    parser.add_argument("--output", type=Path, default=Path("build/differentiation.json"))
    parser.add_argument("--corpus", action="store_true", help="also classify every pinned impulse program")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    binary = (args.build / "benchmark/faustlens_differentiate").resolve()

    def command(*argv, cwd=root):
        return subprocess.check_output(argv, cwd=cwd, text=True).strip()

    cache = {}
    for line in (args.build / "CMakeCache.txt").read_text().splitlines():
        if ":" in line and "=" in line and not line.startswith(("//", "#")):
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    if cache.get("CMAKE_BUILD_TYPE") != "Release":
        parser.error("timing comparisons require a Release build")
    jobs = []
    for source in sorted((root / "benchmark/dsp").glob("*.dsp")):
        jobs.extend((source, mode, "all") for mode in ("plain", "columns", "jvp"))
    source = root / "benchmark/ad/filterbank.dsp"
    jobs.append((source, "plain", "0"))
    jobs.extend((source, "columns", str(n)) for n in (0, 1, 4, 16))
    jobs.append((source, "jvp", "16"))
    source = root / "benchmark/ad/mutable_table.dsp"
    jobs.extend((source, mode, "all") for mode in ("plain", "columns", "jvp"))
    if args.corpus:
        jobs.extend((p, "columns", "all") for p in sorted((root / "lib/faust/tests/impulse-tests/dsp").glob("*.dsp")))
    report = {
        "schema": 1,
        "date": datetime.now(timezone.utc).isoformat(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "cpu": command("sysctl", "-n", "machdep.cpu.brand_string"),
        "commit": command("git", "rev-parse", "HEAD"),
        "dirty": bool(command("git", "status", "--porcelain")),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "compiler": command(cache["CMAKE_CXX_COMPILER"], "--version"),
        "build_type": cache["CMAKE_BUILD_TYPE"],
        "compiler_flags": {key: value for key, value in cache.items() if key.startswith("CMAKE_CXX_FLAGS")},
        "faustlibraries_commit": command("git", "rev-parse", "HEAD", cwd=root / "lib/faust/libraries"),
        "settings": {"sample_rate": 48000, "blocks": [64, 256], "precision": "binary64", "controls": "defaults", "input_offset": 0.25,
                     "validation_frames": 48000, "warmup_ms": 100, "timing_batches": 9, "minimum_batch_frames": 65536,
                     "compile_scope": "fresh Session through native publication, loaded root source", "denormals": "default environment"},
        "results": [],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for source, mode, count in jobs:
        result = {"source": str(source), "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(), "mode": mode, "requested": count}
        try:
            process = subprocess.run([str(binary), str(source), mode, count], capture_output=True, text=True, timeout=60)
            if process.returncode:
                result.update(status="failed", error=process.stderr.strip(), stdout=process.stdout)
            else:
                result.update(json.loads(process.stdout))
        except (subprocess.TimeoutExpired, json.JSONDecodeError) as exc:
            result.update(status="failed", error=str(exc))
        report["results"].append(result)
        print(f"{source.name:28} {mode:8} {count:3} {result['status']}", flush=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    failures = [r for r in report["results"] if r["status"] == "failed"]
    print(f"{len(report['results'])} measurements, {len(failures)} failures; report: {args.output}")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
