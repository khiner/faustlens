"""Measure compiler images, generated code, and retained runtime allocations."""
import datetime
import json
import pathlib
import re
import shutil
import struct
import subprocess
import tempfile

from run import ROOT, output


def object_sizes(path):
    data = path.read_bytes()
    magic, cpu, _, kind, commands, _, _, _ = struct.unpack_from("<8I", data)
    if (magic, cpu, kind) != (0xFEEDFACF, 0x100000C, 1):
        raise ValueError("expected an ARM64 Mach-O object")
    metadata = pathlib.Path(str(path) + ".json").read_bytes() + b"\0"
    code, instructions, static, unwind, metadata_matches = 0, 0, 0, 0, 0
    sections = {}
    offset = 32
    for _ in range(commands):
        command, size = struct.unpack_from("<II", data, offset)
        if size < 8 or offset + size > len(data):
            raise ValueError("invalid Mach-O load command")
        if command == 0x19:
            count = struct.unpack_from("<I", data, offset + 64)[0]
            if 72 + count * 80 > size:
                raise ValueError("invalid Mach-O section count")
            for i in range(count):
                name, segment, _, length, at, _, _, _, flags, _, _, _ = struct.unpack_from("<16s16sQQ8I", data, offset + 72 + i * 80)
                name, segment = (value.split(b"\0", 1)[0].decode() for value in (name, segment))
                sections[segment + "," + name] = length
                if segment == "__TEXT":
                    if at + length > len(data):
                        raise ValueError("truncated Mach-O text section")
                    metadata_matches += data[at:at + length].count(metadata)
                    if name in ("__eh_frame", "__unwind_info"):
                        unwind += length
                    else:
                        code += length
                        if flags & 0x80000400:
                            instructions += length
                elif segment in ("__DATA", "__DATA_CONST"):
                    static += length
        offset += size
    if metadata_matches != 1 or not instructions or code < len(metadata):
        raise ValueError("cannot separate LLVM code from its JSON metadata")
    return {"code_bytes": code - len(metadata), "instruction_bytes": instructions, "static_data_bytes": static,
            "metadata_bytes": len(metadata), "unwind_bytes": unwind, "object_bytes": len(data), "sections": sections}


def compiler_images(build, backends, digest):
    images = {}
    for backend in backends:
        mode = "native" if backend == "native" else "llvm"
        if mode in images:
            continue
        binary = build / "benchmark" / ("faustlens_source_" + mode)
        dependencies = {}
        pending = [binary.resolve()]
        while pending:
            image = pending.pop()
            rpaths = re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset", output("otool", "-l", str(image)))
            for line in output("otool", "-L", str(image)).splitlines()[1:]:
                name = line.strip().split(" (", 1)[0]
                if name.startswith(("/usr/lib/", "/System/")):
                    continue
                dependency = pathlib.Path(name.replace("@loader_path", str(image.parent)))
                if name.startswith("@rpath/"):
                    candidates = [pathlib.Path(p.replace("@loader_path", str(image.parent))) / name.removeprefix("@rpath/")
                                  for p in (str(image.parent), *rpaths)]
                    dependency = next((p for p in candidates if p.is_file()), dependency)
                dependency = dependency.resolve(strict=True)
                if dependency == image or str(dependency) in dependencies:
                    continue
                dependencies[str(dependency)] = {"bytes": dependency.stat().st_size, "sha256": digest(dependency)}
                pending.append(dependency)
        with tempfile.TemporaryDirectory() as directory:
            stripped = pathlib.Path(directory) / binary.name
            shutil.copyfile(binary, stripped)
            subprocess.run(["strip", "-S", "-x", str(stripped)], check=True, capture_output=True)
            stripped_bytes = stripped.stat().st_size
        images[mode] = {"executable_bytes": binary.stat().st_size, "stripped_executable_bytes": stripped_bytes,
                        "sha256": digest(binary), "dependencies": dependencies,
                        "distribution_bytes": stripped_bytes + sum(d["bytes"] for d in dependencies.values())}
    return images


def measure(report, build, destination, digest):
    provenance = {"utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "revision": output("git", "rev-parse", "HEAD"),
                  "worktree_status": output("git", "status", "--short"),
                  "binary_sha256": {mode: digest(build / "benchmark" / ("faustlens_memory_" + mode))
                                    for mode in ("native", "llvm") if mode == "native" or "llvm-scalar" in report["backends"]}}
    report["footprint"] = {"provenance": provenance, "compiler_images": compiler_images(build, report["backends"], digest), "measurements": []}
    with tempfile.TemporaryDirectory() as directory:
        obj = pathlib.Path(directory) / "dsp.o"
        for index, timing in enumerate(report["measurements"], 1):
            case, backend = timing["case"], timing["backend"]
            row = {"case": case, "backend": backend}
            try:
                binary = build / "benchmark" / ("faustlens_memory_native" if backend == "native" else "faustlens_memory_llvm")
                process = subprocess.run([str(binary), str(ROOT / "lib/faust/tests/impulse-tests/dsp" / (case + ".dsp")), backend, str(obj)],
                                         cwd=ROOT, capture_output=True, text=True, timeout=120)
                if process.returncode:
                    raise ValueError(f"exit {process.returncode}: {process.stderr.strip()}")
                measured = json.loads(process.stdout)
                for key in ("case", "backend", "inputs", "outputs"):
                    if measured[key] != timing[key]:
                        raise ValueError(f"footprint/timing mismatch: {key}")
                row = measured
                if backend == "native":
                    if row["executable_allocation_bytes"] != timing["code_bytes"]:
                        raise ValueError("native generated size differs from timed program")
                    row["static_data_bytes"] = 0
                else:
                    if row["options"] != timing["options"]:
                        raise ValueError("LLVM options differ from timed program")
                    row.update(object_sizes(obj))
            except (OSError, ValueError, KeyError, struct.error, subprocess.TimeoutExpired) as error:
                row["error"] = str(error)
            report["footprint"]["measurements"].append(row)
            destination.write_text(json.dumps(report, indent=2) + "\n")
            print(f"Footprint {index}/{len(report['measurements'])}: {case} {backend}", flush=True)


def check(report):
    expected = {(row["case"], row["backend"]) for row in report["measurements"]}
    seen = set()
    for row in report["footprint"]["measurements"]:
        key = row["case"], row["backend"]
        if "error" in row:
            raise ValueError(f"footprint failed: {key}: {row['error']}")
        if key not in expected or key in seen:
            raise ValueError(f"unexpected footprint: {key}")
        seen.add(key)
        for field in ("code_bytes", "retained_factory_heap_bytes", "instance_heap_delta_bytes", "runtime_resident_bytes"):
            if not isinstance(row[field], int) or row[field] <= 0:
                raise ValueError(f"invalid {field}: {key}")
        if row["backend"] == "native":
            if not 0 <= row["executable_allocation_bytes"] - row["code_bytes"] < 16:
                raise ValueError(f"invalid executable alignment: {key}")
        else:
            text = sum(size for section, size in row["sections"].items() if section.startswith("__TEXT,"))
            if row["code_bytes"] + row["metadata_bytes"] + row["unwind_bytes"] != text or not 0 < row["instruction_bytes"] <= row["code_bytes"]:
                raise ValueError(f"invalid LLVM section sizes: {key}")
        if [r["block"] for r in row["allocations"]] != report["settings"]["blocks"]:
            raise ValueError(f"missing allocation probe: {key}")
        if any(r[field] < 0 for r in row["allocations"] for field in ("first", "next_32")):
            raise ValueError(f"invalid allocation count: {key}")
    if seen != expected:
        raise ValueError("missing footprint measurements")
    images = report["footprint"]["compiler_images"]
    if set(images) != {"native" if backend == "native" else "llvm" for backend in report["backends"]}:
        raise ValueError("missing compiler image measurements")
    for image in images.values():
        size = image["stripped_executable_bytes"] + sum(d["bytes"] for d in image["dependencies"].values())
        if image["distribution_bytes"] != size or image["stripped_executable_bytes"] <= 0:
            raise ValueError("invalid compiler distribution size")
