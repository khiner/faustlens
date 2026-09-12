#!/usr/bin/env python3
"""Record isolated DSP measurements and build provenance."""
import argparse
import datetime
import hashlib
import json
import pathlib
import platform
import selectors
import shutil
import subprocess
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]


def output(*args):
    return subprocess.check_output(args, cwd=ROOT, text=True).strip()


def write_report(report, path):
    collections = [key for key in ("measurements", "jit_measurements", "jit_cold_startup") if key in report]
    text = json.dumps({key: value for key, value in report.items() if key not in collections}, indent=2)[:-2]
    for key in collections:
        text += ',\n  "' + key + '": [\n'
        text += ",\n".join("    " + json.dumps(row) for row in report[key])
        text += "\n  ]"
    path.write_text(text + "\n}\n")


def cold_process(command):
    with tempfile.TemporaryFile(mode="w+t") as errors:
        start = time.perf_counter_ns()
        with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=errors, text=True) as process:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                if not selector.select(timeout=120):
                    process.kill()
                    raise RuntimeError("JIT startup timed out")
            line = process.stdout.readline()
            ready_ms = (time.perf_counter_ns() - start) / 1e6
            process.wait(timeout=30)
            if process.returncode or not line:
                errors.seek(0)
                raise RuntimeError(f"JIT startup failed: {errors.read()}")
    return {**json.loads(line), "process_to_first_block_ms": ready_ms}


def jit_comparison(args, report):
    binaries = {mode: args.build.resolve() / "benchmark" / ("faustlens_jit_" + mode) for mode in ("native", "llvm")}
    cache = (args.build / "CMakeCache.txt").read_text().splitlines()
    library = pathlib.Path(next(line.split("=", 1)[1] for line in cache if line.startswith("FAUSTLENS_LLVM_LIBRARY:FILEPATH="))).resolve()
    dependencies = [library]
    for line in output("otool", "-L", str(library)).splitlines()[1:]:
        dependency = pathlib.Path(line.strip().split(" (", 1)[0])
        if "LLVM" in dependency.name and dependency.is_file():
            dependencies.append(dependency.resolve())
    report["jit"] = {
        "binary_sha256": {mode: hashlib.sha256(binary.read_bytes()).hexdigest() for mode, binary in binaries.items()},
        "library_sha256": {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in dependencies},
        "library_bytes": {str(path): path.stat().st_size for path in dependencies},
        "binary_bytes": {mode: binary.stat().st_size for mode, binary in binaries.items()},
        "uncached_compilations_after_first": 11, "instance_creations": 11, "cache_hits": 11,
        "cold_processes_per_case_backend": 11, "cold_block": 64,
        "faust_opt": "FAUST_LLVM_NO_FM", "llvm_optimization_level": -1,
    }
    baseline = json.loads(args.jit_baseline.read_text()) if args.jit_baseline else None
    if baseline:
        for key in ("hardware", "sample_rate", "render_batches", "render_warmup_ms", "thread_qos", "faust_revision", "libraries_revision"):
            if baseline[key] != report[key]:
                raise ValueError(f"LLVM baseline mismatch: {key}")
        for key in ("library_sha256", "faust_opt", "llvm_optimization_level"):
            if baseline["jit"][key] != report["jit"][key]:
                raise ValueError(f"LLVM baseline mismatch: {key}")
        if baseline["jit"]["binary_sha256"]["llvm"] != report["jit"]["binary_sha256"]["llvm"]:
            raise ValueError("LLVM baseline binary mismatch")
        if any(baseline["source_sha256"].get(name) != report["source_sha256"][name] for name in args.cases):
            raise ValueError("LLVM baseline source mismatch")
        report["jit"]["reused_llvm"] = {
            "report": args.jit_baseline.name, "utc": baseline["utc"],
            "sha256": hashlib.sha256(args.jit_baseline.read_bytes()).hexdigest(),
        }
    report["jit_measurements"] = []
    report["jit_cold_startup"] = []
    for name in args.cases:
        for backend in ("native", "llvm-scalar", "llvm-vector"):
            if baseline and backend != "native":
                rows = [r for r in baseline["jit_measurements"] if r["case"] == name and r["backend"] == backend and r["block"] in args.blocks]
                cold = [r for r in baseline["jit_cold_startup"] if r["case"] == name and r["backend"] == backend]
                if {r["block"] for r in rows} != set(args.blocks) or len(cold) != 11:
                    raise ValueError(f"Incomplete LLVM baseline: {name}/{backend}")
                report["jit_measurements"].extend(rows)
                report["jit_cold_startup"].extend(cold)
                write_report(report, args.output)
                continue
            binary = binaries["native" if backend == "native" else "llvm"]
            for block in args.blocks:
                result = subprocess.run([str(binary), name, backend, str(block)], text=True, capture_output=True, timeout=120)
                if result.returncode:
                    raise RuntimeError(f"JIT {name}/{backend}/{block}: {result.stderr}\n{result.stdout}")
                row = json.loads(result.stdout)
                report["jit_measurements"].append(row)
                print(f"JIT {name}/{backend}/{block}: {row['compile_p50_ms']:.3f} ms compile, {row['render_p50_ns_per_frame']:.3f} ns/frame", flush=True)
                write_report(report, args.output)
            for sample in range(11):
                row = cold_process([str(binary), name, backend, "64", "--cold"])
                report["jit_cold_startup"].append({"case": name, "backend": backend, "sample": sample, **row})
            write_report(report, args.output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=pathlib.Path, default=ROOT / "build")
    parser.add_argument("--output", type=pathlib.Path, default=ROOT / "build/backend-current.json")
    parser.add_argument("--blocks", type=int, nargs="+", default=[32, 64, 256])
    parser.add_argument("--cases", nargs="+", default=[p.stem for p in sorted((ROOT / "benchmark/dsp").glob("*.dsp"))])
    parser.add_argument("--jit", action="store_true", help="Compare native and Faust LLVM JIT compilation and execution")
    parser.add_argument("--jit-baseline", type=pathlib.Path, help="Reuse saved LLVM JIT measurements while measuring native execution")
    args = parser.parse_args()
    args.jit = args.jit or args.jit_baseline is not None
    build = args.build.resolve()
    binary = build / "benchmark/faustlens_benchmark"
    if args.jit:
        for mode in ("native", "llvm"):
            if not (build / "benchmark" / ("faustlens_jit_" + mode)).is_file():
                parser.error("build the optional JIT benchmarks with FAUSTLENS_LLVM_LIBRARY; see benchmark/README.md")
    cache = dict(line.split("=", 1) for line in (build / "CMakeCache.txt").read_text().splitlines() if "=" in line and not line.startswith(("#", "//")))
    compiler = cache["CMAKE_CXX_COMPILER:STRING"]
    source_digest = hashlib.sha256()
    for name in sorted(output("git", "ls-files", "--cached", "--others", "--exclude-standard").splitlines()):
        source = ROOT / name
        if source.is_file() and (source.suffix in {".cpp", ".h", ".cmake", ".py", ".dsp", ".in", ".entitlements"} or source.name == "CMakeLists.txt"):
            source_digest.update(name.encode() + b"\0" + source.read_bytes() + b"\0")
    report = {
        "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "revision": output("git", "rev-parse", "HEAD"),
        "source_tree_sha256": source_digest.hexdigest(),
        "faust_revision": output("git", "-C", "lib/faust", "rev-parse", "HEAD"),
        "libraries_revision": output("git", "-C", "lib/faust/libraries", "rev-parse", "HEAD"),
        "compiler": output(compiler, "--version"),
        "platform": platform.platform(),
        "hardware": output("sysctl", "-n", "machdep.cpu.brand_string"),
        "build_type": cache["CMAKE_BUILD_TYPE:STRING"],
        "build_flags": {key: value for key, value in cache.items() if key.startswith("CMAKE_CXX_FLAGS")},
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "sample_rate": 48000,
        "render_batches": 9,
        "render_warmup_ms": 100,
        "thread_qos": "user-initiated" if platform.system() == "Darwin" else "default",
        "native_compilations": 11,
        "source_sha256": {name: hashlib.sha256((ROOT / "benchmark/dsp" / (name + ".dsp")).read_bytes()).hexdigest() for name in args.cases},
        "measurements": [],
        "compilation_scaling": [],
    }
    with tempfile.TemporaryDirectory() as directory:
        sizes = {}
        for backend in ("interp", "native"):
            original = build / "benchmark" / ("faustlens_footprint_" + backend)
            copy = pathlib.Path(directory) / original.name
            shutil.copyfile(original, copy)
            subprocess.run(["strip", "-S", "-x", str(copy)], check=True, capture_output=True)
            sizes[backend] = copy.stat().st_size
        report["stripped_footprint_bytes"] = sizes
    for count in (1000, 10000):
        for shape in ("chain", "wide", "math"):
            result = json.loads(output(str(build / "benchmark/faustlens_compile"), str(count), shape))
            report["compilation_scaling"].append(result)
            print(f"compile {count} {shape}: {result['p95_ms']:.3f} ms p95", flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for name in args.cases:
        for block in args.blocks:
            for backend in ("interp", "scalar", "vector", "native"):
                result = subprocess.run([str(binary), name, backend, str(block)], text=True, capture_output=True)
                if result.returncode:
                    raise RuntimeError(f"{name}/{backend}/{block}: {result.stderr}\n{result.stdout}")
                record = json.loads(result.stdout)
                report["measurements"].append(record)
                print(f"{name:12} {block:4} {backend:7} {record['render_p50_ns_per_frame']:.2f} ns/frame", flush=True)
                write_report(report, args.output)
    if args.jit:
        jit_comparison(args, report)


if __name__ == "__main__":
    main()
