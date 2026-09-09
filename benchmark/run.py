#!/usr/bin/env python3
"""Record isolated DSP measurements and build provenance."""
import argparse
import datetime
import hashlib
import json
import pathlib
import platform
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def output(*args):
    return subprocess.check_output(args, cwd=ROOT, text=True).strip()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=pathlib.Path, default=ROOT / "build")
    parser.add_argument("--output", type=pathlib.Path, default=ROOT / "build/backend-baseline.json")
    parser.add_argument("--blocks", type=int, nargs="+", default=[32, 64, 256])
    parser.add_argument("--cases", nargs="+", default=[p.stem for p in sorted((ROOT / "benchmark/dsp").glob("*.dsp"))])
    args = parser.parse_args()
    build = args.build.resolve()
    binary = build / "benchmark/faustlens_benchmark"
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
        "native_compilations": 11,
        "edits": 9,
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
                if record["unsupported"]:
                    for key in record:
                        if key.startswith(("native_compile_", "render_", "edit_")):
                            record[key] = None
                report["measurements"].append(record)
                print(f"{name:12} {block:4} {backend:7} {record['unsupported'] or str(round(record['render_p50_ns_per_frame'], 2)) + ' ns/frame'}", flush=True)
                metadata = {key: value for key, value in report.items() if key != "measurements"}
                text = json.dumps(metadata, indent=2)[:-2] + ',\n  "measurements": [\n'
                text += ",\n".join("    " + json.dumps(row) for row in report["measurements"])
                args.output.write_text(text + "\n  ]\n}\n")


if __name__ == "__main__":
    main()
