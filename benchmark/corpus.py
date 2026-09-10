#!/usr/bin/env python3
"""Compare compilation, DSP execution, and memory across the pinned impulse corpus."""
import argparse
import array
import datetime
import hashlib
import json
import math
import pathlib
import platform
import statistics
import subprocess

import footprint
from run import ROOT, output

CORPUS = ROOT / "lib/faust/tests/impulse-tests/dsp"
BACKENDS = ("native", "llvm-scalar", "llvm-vector")


def digest(path):
    with path.open("rb") as file:
        return hashlib.file_digest(file, "sha256").hexdigest()


def sources(directory, suffixes):
    return {str(p.relative_to(directory)): digest(p) for p in sorted(directory.rglob("*")) if p.suffix in suffixes and p.is_file()}


def trace_values(path, row):
    if digest(path) != row["trace_sha256"]:
        raise ValueError(f"trace hash mismatch: {path}")
    values = array.array("d")
    values.frombytes(path.read_bytes())
    if len(values) != row["outputs"] * 48000 * 2:
        raise ValueError(f"trace length mismatch: {path}")
    return values


def compare(rows, directory):
    reference = next((row for row in rows if row["backend"] == "llvm-scalar" and "error" not in row), None)
    oracle = trace_values(directory / reference["trace"], reference) if reference else None
    for row in rows:
        if "error" in row:
            continue
        values = trace_values(directory / row["trace"], row)
        row["nonfinite"] = sum(not math.isfinite(value) for value in values)
        if oracle is None or row is reference:
            continue
        if (row["inputs"], row["outputs"]) != (reference["inputs"], reference["outputs"]):
            row["error"] = "channel counts differ from LLVM scalar"
            continue
        maximum, scaled, squared = 0.0, 0.0, 0.0
        for a, b in zip(values, oracle):
            error = abs(a - b)
            maximum = max(maximum, error)
            scaled = max(scaled, error / max(1.0, abs(b)))
            squared += error * error
        row["accuracy"] = {"compared": len(values), "max_error": maximum, "max_scaled_error": scaled,
                           "rms_error": math.sqrt(squared / len(values))}


def check_row(row, settings, backends):
    key = row["case"], row["backend"]
    if "error" in row:
        raise ValueError(f"benchmark failed for {key}: {row['error']}")
    for field in ("compile_ms", "compile_peak_rss_bytes", "process_peak_rss_bytes", "create_ms", "init_ms"):
        if not math.isfinite(row[field]) or row[field] <= 0:
            raise ValueError(f"invalid {field}: {key}")
    if row["compile_peak_rss_bytes"] > row["process_peak_rss_bytes"] or row["inputs"] < 0 or row["outputs"] < 1 or row["nonfinite"]:
        raise ValueError(f"invalid memory, channels, or output trace: {key}")
    if [r["block"] for r in row["render"]] != settings["blocks"]:
        raise ValueError(f"missing DSP block size: {key}")
    for render in row["render"]:
        samples = render["ns_per_frame"]
        if len(samples) != settings["render_batches"] or any(not math.isfinite(value) or value <= 0 for value in samples):
            raise ValueError(f"invalid DSP timing samples: {key}")
        if render["p50_ns_per_frame"] != statistics.median(samples) or render["p95_ns_per_frame"] != max(samples):
            raise ValueError(f"invalid DSP timing percentiles: {key}")
    if "llvm-scalar" in backends and row["backend"] != "llvm-scalar":
        accuracy = row["accuracy"]
        scaled = accuracy["max_scaled_error"]
        if accuracy["compared"] != row["outputs"] * 48000 * 2 or not math.isfinite(scaled) or scaled > settings["accuracy_scaled_tolerance"]:
            raise ValueError(f"output differs from LLVM scalar: {key}: {accuracy}")
    if row["backend"] == "native":
        stages = [row[field] for field in ("frontend_ms", "emit_ms", "publish_ms")]
        if any(not math.isfinite(value) or value < 0 for value in stages) or sum(stages) > row["compile_ms"] + 1e-6:
            raise ValueError(f"invalid compilation stages: {key}")
        if row["code_bytes"] <= 0 or row["instructions"] <= 0:
            raise ValueError(f"empty compiled program: {key}")
    else:
        options = row["options"].split()
        vector = row["backend"] == "llvm-vector"
        if "-double" not in options or ("-vec" in options) != vector or not row["target"] or not row["faust_version"]:
            raise ValueError(f"invalid LLVM configuration: {key}")
        if vector and ("-vs", "32") not in zip(options, options[1:]):
            raise ValueError(f"invalid LLVM vector size: {key}")


def check(report):
    cases, backends = report["cases"], report["backends"]
    if not cases or len(set(cases)) != len(cases) or backends not in (["native"], list(BACKENDS[:2]), list(BACKENDS)):
        raise ValueError("invalid corpus or backend selection")
    if cases != sorted(pathlib.Path(p).stem for p in report["inputs"]["corpus"] if p.endswith(".dsp")):
        raise ValueError("case list differs from the recorded corpus")
    expected = {(case, backend) for case in cases for backend in backends}
    seen = set()
    for row in report["measurements"]:
        key = row["case"], row["backend"]
        if key not in expected or key in seen:
            raise ValueError(f"unexpected or duplicate measurement: {key}")
        seen.add(key)
        check_row(row, report["settings"], backends)
    if seen != expected:
        raise ValueError(f"missing measurements: {sorted(expected - seen)}")
    if "footprint" in report:
        footprint.check(report)


def check_baseline(baseline, report):
    check(baseline)
    if "llvm-scalar" not in baseline["backends"]:
        raise ValueError("baseline must include LLVM scalar")
    for key in ("inputs", "settings", "hardware", "platform", "llvm"):
        if baseline.get(key) != report[key]:
            raise ValueError(f"LLVM baseline mismatch: {key}")


def markdown(report):
    backends, cases = report["backends"], report["cases"]
    rows = {(r["case"], r["backend"]): r for r in report["measurements"]}
    sizes = {(r["case"], r["backend"]): r for r in report.get("footprint", {}).get("measurements", [])}
    lines = ["# Impulse corpus comparison", "", "Values in paired cells: " + " / ".join(backends) + ".", "",
             "DSP times are microseconds per block at 48 kHz; p95 is the maximum of nine batch-average observations.", ""]
    lines += ["Timing measurements: " + report["utc"] + ".", ""]
    if sizes:
        lines += ["Size and memory measurements: " + report["footprint"]["provenance"]["utc"] + ".", ""]

    def table(title, headers, values):
        lines.extend(["## " + title, "", "| " + " | ".join(headers) + " |", "|" + "---|" * len(headers)])
        lines.extend("| " + " | ".join(str(value) for value in row) + " |" for row in values)
        lines.append("")

    def paired(case, metric, collection=rows):
        cells = []
        for backend in backends:
            row = collection.get((case, backend), {})
            try:
                cells.append("FAIL" if "error" in row else f"{metric(row):.3f}")
            except KeyError:
                cells.append("unmeasured")
        return " / ".join(cells)

    def dsp_time(row, index, percentile="p50"):
        render = row["render"][index]
        return render[percentile + "_ns_per_frame"] * render["block"] / 1000

    def quantile(values, q):
        return sorted(values)[max(0, math.ceil(q * len(values)) - 1)]

    problem = None
    try:
        check(report)
    except (ValueError, KeyError) as error:
        problem = str(error)
    if problem:
        lines += ["Validation failed: " + problem, ""]
    else:
        summary = []

        def aggregate(label, metric, reduce=statistics.median, collection=rows, suffix=""):
            summary.append([label, *(f"{reduce([metric(collection[case, backend]) for case in cases]):.3f}{suffix}" for backend in backends)])

        for label, reduce in (("Total", sum), ("Median", statistics.median), ("p95", lambda a: quantile(a, .95)), ("Maximum", max)):
            aggregate(label + " compilation ms", lambda r: r["compile_ms"], reduce)
        for stage in ("create", "init"):
            for label, reduce in (("Median", statistics.median), ("p95", lambda a: quantile(a, .95)), ("Maximum", max)):
                aggregate(label + " " + stage + " µs", lambda r, stage=stage: 1000 * r[stage + "_ms"], reduce)
        for i, block in enumerate(report["settings"]["blocks"]):
            for percentile in ("p50", "p95"):
                aggregate(f"Median program DSP {block} batch-{percentile} µs/block", lambda r, i=i, p=percentile: dsp_time(r, i, p))
            aggregate(f"Maximum program DSP {block} batch-p95 µs/block", lambda r, i=i: dsp_time(r, i, "p95"), max)
            if "llvm-scalar" in backends:
                ratios = {backend: [dsp_time(rows[case, backend], i) / dsp_time(rows[case, "llvm-scalar"], i) for case in cases]
                          for backend in backends}
                for label, reduce in (("Geometric mean", statistics.geometric_mean), ("Median", statistics.median), ("Worst", max)):
                    summary.append([f"{label} DSP {block} time relative to LLVM scalar", *(f"{reduce(ratios[b]):.3f}×" for b in backends)])
                summary.append([f"DSP {block} faster / equal / slower than LLVM scalar",
                                *(f"{sum(v < 1 for v in ratios[b])} / {sum(v == 1 for v in ratios[b])} / {sum(v > 1 for v in ratios[b])}" for b in backends)])
        for field, label in (("compile_peak_rss_bytes", "compilation peak"), ("process_peak_rss_bytes", "whole-process peak")):
            for name, reduce in (("Median", statistics.median), ("Maximum", max)):
                aggregate(f"{name} {label} MiB", lambda r, field=field: r[field] / 2**20, reduce)
        if sizes:
            for label, reduce in (("Total", sum), ("Median", statistics.median), ("p95", lambda a: quantile(a, .95)), ("Maximum", max)):
                aggregate(label + " generated code + constants KiB", lambda r: r["code_bytes"] / 1024, reduce, sizes)
            for field, label in (("retained_factory_heap_bytes", "retained factory heap KiB"),
                                 ("instance_heap_delta_bytes", "instance heap KiB"), ("static_data_bytes", "static DSP data KiB"),
                                 ("runtime_resident_bytes", "initialized process resident MiB")):
                for name, reduce in (("Median", statistics.median), ("Maximum", max)):
                    aggregate(name + " " + label, lambda r, f=field: r[f] / (2**20 if f == "runtime_resident_bytes" else 1024), reduce, sizes)
            aggregate("Heap allocation calls across all DSP probes", lambda r: sum(a["first"] + a["next_32"] for a in r["allocations"]), sum, sizes)
        table("Summary across programs", ["Measurement", *backends], summary)
        lines += ["Compilation percentiles describe the distribution across programs, with one sample per program.",
                  "DSP ratios weight every program equally; lower is faster, and win counts are observations without significance thresholds.", ""]

    timings = []
    for case in cases:
        native = rows[case, "native"]
        try:
            for backend in backends:
                check_row(rows[case, backend], report["settings"], backends)
            accuracy = "pass" if "llvm-scalar" in backends else "finite only"
        except (ValueError, KeyError):
            accuracy = "FAIL"
        channels = f"{native['inputs']}/{native['outputs']}" if "inputs" in native else "FAIL"
        timings.append([case, channels, paired(case, lambda r: r["compile_ms"]),
                        *(paired(case, lambda r, i=i, p=p: dsp_time(r, i, p)) for i in range(2) for p in ("p50", "p95")),
                        paired(case, lambda r: 1000 * r["create_ms"]), paired(case, lambda r: 1000 * r["init_ms"]),
                        paired(case, lambda r: r["process_peak_rss_bytes"] / 2**20), accuracy])
    table("Per-program timings", ["Program", "In/out", "Compile ms", "DSP 64 p50 µs", "DSP 64 p95 µs", "DSP 256 p50 µs", "DSP 256 p95 µs",
                                 "Create µs", "Init µs", "Peak MiB", "Accuracy"], timings)
    if sizes:
        memory = []
        for case in cases:
            memory.append([case, *(paired(case, lambda r, f=f: r[f] / 1024, sizes)
                                  for f in ("code_bytes", "retained_factory_heap_bytes", "instance_heap_delta_bytes", "static_data_bytes")),
                           paired(case, lambda r: r["runtime_resident_bytes"] / 2**20, sizes),
                           paired(case, lambda r: sum(a["first"] + a["next_32"] for a in r["allocations"]), sizes)])
        table("Per-program code and memory", ["Program", "Code + constants KiB", "Factory heap KiB", "Instance heap KiB", "Static DSP data KiB",
                                              "Initialized resident MiB", "DSP allocations"], memory)
        table("Compiler/runtime executable + required libraries",
              ["Backend", "Executable MiB", "Stripped executable MiB", "Non-system dependencies MiB", "Total MiB"],
              [[name, f"{image['executable_bytes'] / 2**20:.3f}", f"{image['stripped_executable_bytes'] / 2**20:.3f}",
                f"{sum(d['bytes'] for d in image['dependencies'].values()) / 2**20:.3f}", f"{image['distribution_bytes'] / 2**20:.3f}"]
               for name, image in report["footprint"]["compiler_images"].items()])
        lines += ["Distribution size includes a stripped benchmark client and its current non-system shared libraries, excluding system libraries.",
                  "Factory heap excludes executable mappings; code size excludes alignment, unwind data, and LLVM's separately counted JSON metadata.",
                  "Instance heap includes controls and sound fixtures; LLVM shared static tables are counted in static DSP data.",
                  "Initialized resident memory includes the loaded compiler and allocator caches, with temporary frontend objects released.", ""]
    if not problem and "llvm-scalar" in backends:
        regressions = []
        for i, block in enumerate(report["settings"]["blocks"]):
            for backend in backends:
                if backend == "llvm-scalar":
                    continue
                ordered = sorted(cases, key=lambda c: dsp_time(rows[c, backend], i) / dsp_time(rows[c, "llvm-scalar"], i), reverse=True)
                for case in ordered[:5]:
                    actual, reference = dsp_time(rows[case, backend], i), dsp_time(rows[case, "llvm-scalar"], i)
                    regressions.append([case, backend, block, f"{actual:.3f}", f"{reference:.3f}", f"{actual / reference:.3f}×"])
        table("Largest DSP time ratios", ["Program", "Backend", "Block", "Backend µs", "LLVM scalar µs", "Ratio"], regressions)
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=pathlib.Path, default=ROOT / "build")
    parser.add_argument("--output", type=pathlib.Path, default=ROOT / "build/impulse-benchmark.json")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--llvm", action="store_true", help="also measure LLVM scalar compilation and execution")
    mode.add_argument("--llvm-vector", action="store_true", help="also measure LLVM scalar and vector execution; require every program to succeed")
    mode.add_argument("--llvm-baseline", type=pathlib.Path, help="reuse LLVM measurements from a compatible corpus report")
    mode.add_argument("--check", type=pathlib.Path, help="validate a saved report without running compilers")
    mode.add_argument("--footprint", type=pathlib.Path, help="add code size and memory measurements to saved timings without retiming DSP")
    args = parser.parse_args()
    if args.check:
        check(json.loads(args.check.read_text()))
        print("Complete corpus coverage and valid compiler/runtime measurements")
        return
    if args.llvm_baseline and args.output.resolve() == args.llvm_baseline.resolve():
        parser.error("output must differ from the LLVM baseline")
    build = args.build.resolve()
    cache = dict(line.split("=", 1) for line in (build / "CMakeCache.txt").read_text().splitlines() if "=" in line and not line.startswith(("#", "//")))
    if cache["CMAKE_BUILD_TYPE:STRING"] != "Release":
        parser.error("use a Release build")
    libraries = pathlib.Path(cache["FAUSTLENS_FAUSTLIBRARIES:PATH"])
    if args.footprint:
        report = json.loads(args.footprint.read_text())
        check(report)
        if report["inputs"]["corpus"] != sources(CORPUS, {".dsp", ".lib"}) or report["inputs"]["libraries"] != sources(libraries, {".lib"}):
            parser.error("saved timings use different sources")
        if report["hardware"] != output("sysctl", "-n", "machdep.cpu.brand_string") or report["platform"] != platform.platform():
            parser.error("saved timings use a different machine or OS")
        for name, expected in report.get("llvm", {}).get("library_sha256", {}).items():
            if digest(pathlib.Path(name)) != expected:
                parser.error("saved timings use different LLVM libraries")
        origin = {"path": str(args.footprint), "sha256": digest(args.footprint)}
        for row in report["measurements"]:
            row["trace"] = str((args.footprint.parent / row["trace"]).resolve())
        args.output.parent.mkdir(parents=True, exist_ok=True)
        footprint.measure(report, build, args.output, digest)
        report["footprint"]["provenance"]["timing_report"] = origin
        args.output.write_text(json.dumps(report, indent=2) + "\n")
        args.output.with_suffix(".md").write_text(markdown(report))
        check(report)
        print(f"Added footprint measurements; table: {args.output.with_suffix('.md')}")
        return
    baseline = json.loads(args.llvm_baseline.read_text()) if args.llvm_baseline else None
    backends = list(BACKENDS[:3 if args.llvm_vector else 2 if args.llvm else 1])
    if baseline:
        backends = baseline["backends"]
    binaries = {backend: build / "benchmark" / ("faustlens_source_native" if backend == "native" else "faustlens_source_llvm") for backend in backends}
    report = {
        "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "revision": output("git", "rev-parse", "HEAD"),
        "worktree_status": output("git", "status", "--short"),
        "hardware": output("sysctl", "-n", "machdep.cpu.brand_string"),
        "platform": platform.platform(),
        "compiler": output(cache["CMAKE_CXX_COMPILER:STRING"], "--version"),
        "build_flags": {key: value for key, value in cache.items() if key.startswith("CMAKE_CXX_FLAGS")},
        "inputs": {
            "faust_revision": output("git", "-C", "lib/faust", "rev-parse", "HEAD"),
            "libraries_revision": output("git", "-C", str(libraries), "rev-parse", "HEAD"),
            "corpus": sources(CORPUS, {".dsp", ".lib"}),
            "libraries": sources(libraries, {".lib"}),
        },
        "settings": {
            "measurement": "compile-and-render", "samples_per_program_backend": 1,
            "fresh_process": True, "root_source_preloaded": True, "thread_qos": "user-initiated",
            "precision": "binary64", "vector_size": 32, "llvm_optimization_level": -1,
            "faust_opt": "FAUST_LLVM_NO_FM", "build_type": "Release",
            "sample_rate": 48000, "blocks": [64, 256], "render_batches": 9, "render_warmup_ms": 100,
            "frames_per_batch": 65536, "trace_frames_per_block_size": 48000, "input_offset": 0.25,
            "controls": "default sliders and checkboxes; buttons held at one", "sound_fixture": "two-channel 4096-frame sine per part",
            "accuracy_scaled_tolerance": 1e-10, "render_flush_denormals": True,
        },
        "native_binary_sha256": digest(binaries["native"]),
        "cases": sorted(p.stem for p in CORPUS.glob("*.dsp")),
        "backends": backends,
        "measurements": [],
    }
    if len(backends) > 1:
        library = pathlib.Path(cache["FAUSTLENS_LLVM_LIBRARY:FILEPATH"]).resolve()
        dependencies = [library]
        for line in output("otool", "-L", str(library)).splitlines()[1:]:
            dependency = pathlib.Path(line.strip().split(" (", 1)[0])
            if "LLVM" in dependency.name:
                dependencies.append(dependency.resolve(strict=True))
        report["llvm"] = {
            "binary_sha256": digest(binaries["llvm-scalar"]),
            "library_sha256": {str(path): digest(path) for path in dependencies},
        }
    if baseline:
        check_baseline(baseline, report)
        report["reused_llvm"] = {"path": str(args.llvm_baseline), "utc": baseline["utc"], "sha256": digest(args.llvm_baseline)}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    traces = args.output.with_suffix(".traces")
    traces.mkdir(exist_ok=True)
    for index, case in enumerate(report["cases"], 1):
        case_rows = []
        for backend in backends:
            row = {"case": case, "backend": backend}
            try:
                if baseline and backend != "native":
                    row = dict(next(r for r in baseline["measurements"] if r["case"] == case and r["backend"] == backend))
                    row["trace"] = str((args.llvm_baseline.parent / row["trace"]).resolve())
                else:
                    trace = traces / f"{case}.{backend}.f64"
                    result = subprocess.run([str(binaries[backend]), str(CORPUS / (case + ".dsp")), backend, str(trace.resolve())],
                                            cwd=ROOT, capture_output=True, text=True, timeout=120)
                    if result.returncode:
                        raise ValueError(f"exit {result.returncode}: {result.stderr.strip()}")
                    measured = json.loads(result.stdout)
                    if (measured["case"], measured["backend"]) != (case, backend):
                        raise ValueError("unexpected measurement identity")
                    row = measured
                    row["trace"] = str(trace.relative_to(args.output.parent))
                    row["trace_sha256"] = digest(trace)
            except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
                row["error"] = str(error)
            report["measurements"].append(row)
            case_rows.append(row)
        compare(case_rows, args.output.parent)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
        print(f"{index}/{len(report['cases'])} {case}", flush=True)
    footprint.measure(report, build, args.output, digest)
    args.output.with_suffix(".md").write_text(markdown(report))
    check(report)
    print(f"Validated {len(report['measurements'])} compiler/runtime measurements; table: {args.output.with_suffix('.md')}")


if __name__ == "__main__":
    main()
