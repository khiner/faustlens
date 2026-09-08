#!/usr/bin/env bash
# Regenerate the reference oracle from the pinned submodule at double precision.
# Use -I dsp to resolve corpus libraries before installed libraries.
# Reference -norm1 emits signals and undocumented -norm2 emits types.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
faust=$root/lib/faust/build/bin/faust
impulse=$root/lib/faust/tests/impulse-tests
out=${FAUSTLENS_ORACLE_DIR:-$root/build/oracle}
levels=${1:-structural}   # structural | ir | all | compare

if [[ ! -x $faust ]]; then
    cat >&2 <<EOF
Reference Faust is not built. From $root/lib/faust/build:
    cmake -C backends/regular.cmake -DFIR_BACKEND=COMPILER -B faustdir -G Ninja .
    ninja -C faustdir faust
\`regular.cmake\` is the preset that avoids LLVM, but it sets FIR_BACKEND OFF
and \`.fir\` needs \`-lang fir\`; no shipped preset gives FIR without LLVM.
EOF
    exit 1
fi

sha=$(git -C "$root/lib/faust/libraries" rev-parse HEAD)
echo "faust:          $("$faust" --version | head -1)"
echo "faustlibraries: $sha"
echo "output:         $out"
mkdir -p "$out"
echo "$sha" > "$out/faustlibraries.sha"

cd "$impulse"

structural() {
    for dsp in dsp/*.dsp; do
        name=$(basename "$dsp" .dsp)
        # Record missing outputs per level and continue with the remaining programs.
        "$faust" -double -I dsp -e "$dsp" -o "$out/$name.box"     2>>"$out/errors.log" || true
        "$faust" -double -I dsp -norm1 "$dsp" > "$out/$name.sig"  2>>"$out/errors.log" || true
        FAUST_OPT=FAUST_SIG_NO_NORM \
        "$faust" -double -I dsp -norm1 "$dsp" > "$out/$name.nonorm.sig" \
            2>>"$out/errors.log" || true
        "$faust" -double -I dsp -norm2 "$dsp" > "$out/$name.type" 2>>"$out/errors.log" || true
        "$faust" -lang fir -double -I dsp "$dsp" > "$out/$name.fir" \
            2>>"$out/errors.log" || true
        printf .
    done
    echo
    find "$out" -size 0 -delete
}

# Generate the reference four-section impulse protocol at 44100 Hz with 15000 frames per section.
impulse_responses() {
    mkdir -p "$out/ir" "$out/cpp"
    for dsp in dsp/*.dsp; do
        name=$(basename "$dsp" .dsp)
        "$faust" -double -I dsp -i -a archs/impulsearch.cpp "$dsp" -o "$out/cpp/$name.cpp" \
            2>>"$out/errors.log" || { printf x; continue; }
        c++ -O3 -I"$root/lib/faust/architecture" -Iarchs -pthread -std=c++11 \
            "$out/cpp/$name.cpp" -o "$out/cpp/$name" 2>>"$out/errors.log" || { printf x; continue; }
        "$out/cpp/$name" -n 60000 > "$out/ir/$name.ir" 2>>"$out/errors.log" || printf x
        printf .
    done
    echo
}

tools() {
    c++ -O3 -std=c++11 tools/filesCompare.cpp -o "$out/filesCompare"
}

# Compare regenerated dumps with shipped files after normalizing precision-dependent literals.
# Exclude FIR text because precision changes function names; conformance tests compare its structure.
compare() {
    local ok=0 differ=0 absent=0
    for ext in box sig type; do
        ok=0; differ=0; absent=0
        for f in "$out"/*."$ext"; do
            [ -e "$f" ] || continue
            local name; name=$(basename "$f")
            case $name in *.nonorm.sig) continue ;; esac
            local shipped=reference/$name
            if [ ! -f "$shipped" ]; then absent=$((absent + 1)); continue; fi
            if diff -q <(normalize "$f") <(normalize "$shipped") > /dev/null; then
                ok=$((ok + 1))
            else
                differ=$((differ + 1))
                [ -n "${VERBOSE:-}" ] && echo "    differs: $name"
            fi
        done
        printf '  %-5s %3d match, %d differ, %d without a shipped file
'             ".$ext" "$ok" "$differ" "$absent"
    done
    echo "  .fir  skipped -- compared as a projection by the conformance suite"
    if [ -d "$out/ir" ]; then
        ok=0; differ=0; absent=0
        for f in "$out"/ir/*.ir; do
            [ -e "$f" ] || continue
            local shipped=reference/$(basename "$f")
            if [ ! -f "$shipped" ]; then absent=$((absent + 1)); continue; fi
            if "$out/filesCompare" "$f" "$shipped" 2e-06 > /dev/null 2>&1; then
                ok=$((ok + 1))
            else
                differ=$((differ + 1))
                [ -n "${VERBOSE:-}" ] && echo "    differs: $(basename "$f")"
            fi
        done
        printf '  %-5s %3d match, %d differ, %d without a shipped file
'             ".ir" "$ok" "$differ" "$absent"
    fi
}

normalize() {
    grep -v 'declare version\|compile_options\|library_path\|declare filename' "$1" \
      | sed -E 's/[0-9]+\.[0-9]*([eE][-+]?[0-9]+)?f?/N/g; s/[0-9]+[eE][-+]?[0-9]+f?/N/g'
}

: > "$out/errors.log"
case $levels in
    structural) tools; structural ;;
    ir)         tools; impulse_responses ;;
    all)        tools; structural; impulse_responses ;;
    compare)    echo "against the shipped set:"; compare; exit 0 ;;
    *) echo "usage: $0 [structural|ir|all|compare]" >&2; exit 2 ;;
esac

echo "regenerated:"
for ext in box sig type fir; do
    printf '  %-6s %3d (shipped %d)\n' ".$ext" \
        "$(find "$out" -maxdepth 1 -name "*.$ext" ! -name '*.nonorm.sig' | wc -l | tr -d ' ')" \
        "$(find reference -maxdepth 1 -name "*.$ext" | wc -l | tr -d ' ')"
done
[[ -d $out/ir ]] && printf '  %-6s %3d\n' ".ir" "$(find "$out/ir" -name '*.ir' | wc -l | tr -d ' ')"
[[ -s $out/errors.log ]] && echo "  (diagnostics in $out/errors.log)"
exit 0
