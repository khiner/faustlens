#!/usr/bin/env python3
"""Check a complete benchmark report against the native backend acceptance budgets."""
import json
import math
import pathlib
import sys


def main():
    report = json.loads(pathlib.Path(sys.argv[1]).read_text())
    failures = []

    def check(condition, message):
        if not condition:
            failures.append(message)

    sizes = report["stripped_footprint_bytes"]
    check(sizes["native"] - sizes["interp"] <= 1024**2, "native footprint exceeds 1 MiB")
    scaling = report["compilation_scaling"]
    check({(r["instructions"], r["shape"]) for r in scaling} ==
          {(n, shape) for n in (1000, 10000) for shape in ("chain", "wide", "math")}, "compilation scaling cases are incomplete")
    for row in scaling:
        label = f"compile {row['instructions']}/{row['shape']}"
        check(row["p95_ms"] <= row["instructions"] / 1000, f"{label}: compilation exceeds budget")
        check(row["exact"], f"{label}: output differs from the interpreter")
    groups = {}
    for row in report["measurements"]:
        groups.setdefault((row["case"], row["block"]), {})[row["backend"]] = row
    cases = {p.stem for p in (pathlib.Path(__file__).parent / "dsp").glob("*.dsp")}
    check(set(groups) == {(case, block) for case in cases for block in (32, 64, 256)}, "DSP benchmark cases are incomplete")
    for (case, block), rows in groups.items():
        label = f"{case}/{block}"
        check(set(rows) == {"interp", "native", "scalar", "vector"}, f"{label}: backends are incomplete")
        if set(rows) != {"interp", "native", "scalar", "vector"}:
            continue
        native = rows["native"]
        reference = min(rows[backend]["render_p50_ns_per_frame"] for backend in ("scalar", "vector"))
        budget = max(1.5 * reference, reference + 250 / block)
        check(native["render_p50_ns_per_frame"] <= budget, f"{label}: throughput exceeds {budget:.3f} ns/frame")
        check(native["edit_prepare_p95_ms"] <= 10, f"{label}: edit preparation exceeds 10 ms")
        check(native["edit_to_output_p95_ms"] <= 11 + block / 48, f"{label}: edit-to-output exceeds budget")
        check(native["prepare_peak_rss_bytes"] - rows["interp"]["prepare_peak_rss_bytes"] <= 32 * 1024**2,
              f"{label}: preparation memory exceeds budget")
        for backend, row in rows.items():
            check(not row["nonfinite"] and row["max_error"] <= 1e-10 * max(1, row["reference_peak"]),
                  f"{label}/{backend}: reference accuracy exceeds tolerance")
    if "jit" in report:
        backends = {"native", "llvm-scalar", "llvm-vector"}
        rows = report["jit_measurements"]
        expected = {(case, backend, block) for case in cases for backend in backends for block in (32, 64, 256)}
        check(len(rows) == len(expected) and {(r["case"], r["backend"], r["block"]) for r in rows} == expected,
              "JIT benchmark cases are incomplete or duplicated")
        for row in rows:
            label = f"JIT {row['case']}/{row['backend']}/{row['block']}"
            check(not row["nonfinite"] and row["max_error"] <= 1e-10 * max(1, row["reference_peak"]),
                  f"{label}: reference accuracy exceeds tolerance")
            for stage in ("compile", "create", "init"):
                check(0 <= row[f"{stage}_p50_ms"] <= row[f"{stage}_p95_ms"] < math.inf, f"{label}: invalid {stage} timings")
            for field in ("compile_first_ms", "render_p50_ns_per_frame", "render_p95_ns_per_frame"):
                check(0 < row[field] < math.inf, f"{label}: invalid {field}")
            if row["backend"] == "native":
                check(row["cache_hit_p50_ms"] is None and row["cache_hit_p95_ms"] is None, f"{label}: native has no factory lookup cache")
            else:
                check(0 < row["cache_hit_p50_ms"] <= row["cache_hit_p95_ms"] < math.inf, f"{label}: invalid cache-hit timings")
                check("-double" in row["options"] and (("-vec" in row["options"]) == (row["backend"] == "llvm-vector")),
                      f"{label}: unexpected Faust options")
        cold = report["jit_cold_startup"]
        expected = {(case, backend, sample) for case in cases for backend in backends for sample in range(11)}
        check(len(cold) == len(expected) and {(r["case"], r["backend"], r["sample"]) for r in cold} == expected,
              "JIT cold-start cases are incomplete or duplicated")
        for row in cold:
            label = f"cold JIT {row['case']}/{row['backend']}/{row['sample']}"
            stages = [row[key] for key in ("compile_ms", "create_ms", "init_ms", "first_block_ms")]
            check(all(0 <= x < math.inf for x in stages) and sum(stages) <= row["process_to_first_block_ms"] < math.inf,
                  f"{label}: inconsistent startup timing boundaries")
            check(math.isfinite(row["checksum"]), f"{label}: nonfinite first block")
        check(report["jit"]["faust_opt"] == "FAUST_LLVM_NO_FM", "LLVM fast math must be disabled")
        print(f"JIT coverage: {len(rows)} compilation/render cases and {len(cold)} cold processes")
    for message in failures:
        print(message)
    print(f"{'FAIL' if failures else 'PASS'}: {len(groups)} DSP cases, {len(scaling)} compilation cases, footprint, memory, and edit latency")
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
