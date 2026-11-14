#!/usr/bin/env python3
"""Split the generated function-registration TU into smaller functions.

ReXGlue emits `mgsv_gz_RegisterFunctions` as a single function containing one
`registrar->SetFunction(...)` call per recompiled guest function (~68 k for
MGSV:GZ). Apple clang's Localizer pass is quadratic in the number of machine
instructions in a basic block, so that one function takes >12 minutes to
compile no matter the optimisation level.

This rewrites the body into N small per-chunk functions. Registration order is
preserved, the loop-free structure is identical, and compile time drops to
seconds. Safe to re-run: an already-split file is left alone.

    python3 tools/split_register.py port/generated/default/mgsv_gz_register.cpp
"""

import pathlib
import re
import sys

CHUNK = 2000
CALL_RE = re.compile(r"^\s*registrar->SetFunction\((\s*0x[0-9A-Fa-f]+\s*,\s*[A-Za-z_]\w*\s*)\);\s*$")
MARKER = "// split by tools/split_register.py"

HEADER = """//=============================================================================
// ReXGlue Generated - mgsv_gz Function Registration
//=============================================================================
{marker}
#include "mgsv_gz_init.h"
#include <rex/system/function_dispatcher.h>
"""


def main(argv):
    if len(argv) != 2:
        raise SystemExit(f"usage: {argv[0]} <generated register .cpp>")

    path = pathlib.Path(argv[1])
    text = path.read_text()

    if MARKER in text:
        print(f"{path}: already split, nothing to do")
        return 0

    calls = []
    for line in text.splitlines():
        m = CALL_RE.match(line)
        if m:
            calls.append(re.sub(r"\s+", " ", m.group(1)))
    if not calls:
        raise SystemExit(f"{path}: no SetFunction calls found")

    chunks = [calls[i:i + CHUNK] for i in range(0, len(calls), CHUNK)]

    out = [HEADER.format(marker=MARKER), "namespace {\n"]
    for i, chunk in enumerate(chunks):
        out.append(f"void RegisterChunk{i}(rex::runtime::IModuleRegistrar* registrar) {{")
        out.extend(f"  registrar->SetFunction({call});" for call in chunk)
        out.append("}\n")
    out.append("}  // namespace\n")
    out.append(
        "\nvoid mgsv_gz_RegisterFunctions(rex::runtime::IModuleRegistrar* registrar) {")
    out.extend(f"  RegisterChunk{i}(registrar);" for i in range(len(chunks)))
    out.append("}\n")

    path.write_text("\n".join(out))
    print(f"{path}: {len(calls)} registrations -> {len(chunks)} chunks of <= {CHUNK}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
