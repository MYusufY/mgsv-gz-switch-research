#!/usr/bin/env python3
"""Make the generated function-boundary overrides produce resolvable control flow.

tools/find_gaps.py proposes function entries that .pdata does not describe:
vtable slots, `lis`+`addi` pointer constructions, and every thunk boundary in
the gaps that contain them. Some of those regions still contain branch targets
the recompiler cannot resolve -- a `bctr` switch label, a shared epilogue, a
tail-call destination that is also called indirectly.

The fix for an unresolved *target* is to give it a body of its own: declare it
as a function boundary so the branch resolves to a call instead of a trap.
Declaring it also bounds its neighbours, so every range is re-tiled after each
round. Only the target address is used -- the "from" site is just the reporting
instruction and has nothing to do with the missing body.

Repeats until codegen reports no unresolved targets.

    python3 tools/prune_gap_overrides.py port/mgsv_gz_manifest.toml

Hand-written entries above the generated marker are never modified.
"""

import bisect
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import find_gaps

MARKER = "# Entries discovered by tools/find_gaps.py"
ENTRY_RE = re.compile(r'"(0x[0-9A-Fa-f]+)" = \{ size = (0x[0-9A-Fa-f]+) \}')

# Each report names one missing target; capture it and ignore the caller.
UNRESOLVED_PATTERNS = (
    re.compile(r"Jump target (0x[0-9A-Fa-f]{8})"),
    re.compile(r"Unresolved [a-z ]*?target (0x[0-9A-Fa-f]{8})"),
    re.compile(r"Unresolved [a-z ]*?branch to (0x[0-9A-Fa-f]{8})"),
    re.compile(r"Unresolved (?:type )?function (0x[0-9A-Fa-f]{8})"),
)


def load(path):
    text = path.read_text()
    head, sep, auto = text.partition(MARKER)
    if not sep:
        raise SystemExit(f"{path}: no '{MARKER}' section")
    entries = sorted({int(m.group(1), 16) for m in ENTRY_RE.finditer(auto)})
    hand = sorted({int(m.group(1), 16) for m in ENTRY_RE.finditer(head)})
    return head + MARKER, entries, hand


def store(path, head, entries, hand, pdata_starts, text_hi, note):
    """Write entries, tiling each range up to the next known boundary."""
    bounds = sorted(set(pdata_starts) | set(hand) | set(entries))
    rows = []
    for addr in entries:
        i = bisect.bisect_right(bounds, addr)
        nxt = bounds[i] if i < len(bounds) else text_hi
        rows.append((addr, nxt - addr))
    body = "\n".join(f'"0x{a:08X}" = {{ size = 0x{s:X} }}' for a, s in rows)
    path.write_text(f"{head}\n{note}\n{body}\n")


def codegen(manifest, log_path):
    with open(log_path, "w") as log:
        proc = subprocess.run(
            ["rexglue", "codegen", "--ignore-stamp", "--log-level=info",
             str(manifest)],
            stdout=log, stderr=subprocess.STDOUT)
    text = pathlib.Path(log_path).read_text()
    if proc.returncode != 0:
        print(text[-2000:])
        raise SystemExit("codegen failed; fix overrides.toml by hand")
    addrs = set()
    for line in text.splitlines():
        if "unresolved" not in line.lower():
            continue
        for pattern in UNRESOLVED_PATTERNS:
            for h in pattern.findall(line):
                addrs.add(int(h, 16))
    return addrs


def main():
    manifest = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                            else "port/mgsv_gz_manifest.toml")
    overrides = pathlib.Path("port/overrides.toml")
    log_dir = pathlib.Path("port/out")

    img = open("extracted/default.image.bin", "rb").read()
    secs = find_gaps.sections(img)
    text_lo, text_size, _ = secs[".text"]
    text_hi = text_lo + text_size
    funcs = find_gaps.pdata_functions(img, secs, text_lo, text_hi)
    pdata_starts = [f[0] for f in funcs]

    def covered(addr):
        i = bisect.bisect_right(pdata_starts, addr) - 1
        return i >= 0 and addr < funcs[i][0] + funcs[i][1]

    note = (
        "# Entries discovered by tools/find_gaps.py: inside .pdata gaps. Every gap that\n"
        "# contains an address the game actually takes (vtable slot or lis-composed\n"
        "# pointer) is split into individual thunks -- MSVC emits no unwind records for\n"
        "# those forwarder tables, but an indirect call still needs a body. Entries the\n"
        "# recompiler still could not resolve are added by tools/prune_gap_overrides.py.\n")

    for iteration in range(1, 13):
        head, entries, hand = load(overrides)

        # Always re-tile first: sizes may be stale after a discovery run.
        store(overrides, head, entries, hand, pdata_starts, text_hi, note)

        log_path = log_dir / f"codegen-prune-{iteration}.log"
        unresolved = codegen(manifest, log_path)

        known = set(entries) | set(hand) | set(pdata_starts)
        added = sorted(a for a in unresolved
                       if text_lo <= a < text_hi
                       and not (a & 3)
                       and a not in known
                       and not covered(a))
        print(f"pass {iteration}: {len(entries)} entries, "
              f"{len(unresolved)} unresolved targets, adding {len(added)}"
              f" -> {log_path}")

        if not added:
            if unresolved:
                print("no addable targets left; remaining:", 
                      " ".join(f"0x{a:08X}" for a in sorted(unresolved)[:10]))
                return 1
            print("clean")
            return 0

        store(overrides, head, sorted(set(entries) | set(added)), hand,
              pdata_starts, text_hi, note)

    print("still unresolved after 12 passes")
    return 1


if __name__ == "__main__":
    sys.exit(main())
