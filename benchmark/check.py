#!/usr/bin/env python3
"""Check a complete benchmark report against the native backend acceptance budgets."""
import json
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
        check(not native["unsupported"], f"{label}: {native['unsupported']}")
        if native["unsupported"]:
            continue
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
    for message in failures:
        print(message)
    print(f"{'FAIL' if failures else 'PASS'}: {len(groups)} DSP cases, {len(scaling)} compilation cases, footprint, memory, and edit latency")
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
