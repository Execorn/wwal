#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

import os
import sys


def c_escape(line: str) -> str:
    return line.replace("\\", "\\\\").replace('"', '\\"')


def main() -> int:
    if len(sys.argv) < 3:
        sys.stderr.write("Usage: embed_shaders.py <output.h> <shader1.comp> [...]\n")
        return 1

    out_path = sys.argv[1]
    shader_paths = sys.argv[2:]

    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "#ifndef WAYWAL_SHADERS_EMBEDDED_H",
        "#define WAYWAL_SHADERS_EMBEDDED_H",
        "",
    ]

    for path in shader_paths:
        stem = os.path.splitext(os.path.basename(path))[0].upper()
        if stem == "DISSOLVE_NOISE":
            stem = "NOISE"
        var_name = f"SHADER_SRC_{stem}"

        with open(path, "r", encoding="utf-8") as f:
            raw = f.read()

        lines.append(f"static const char *{var_name} =")
        for line in raw.splitlines():
            # Strip standalone SPDX comment lines from embedded GLSL string so #version 310 es stays on line 1
            if line.strip().startswith("// SPDX-License-Identifier:"):
                continue
            lines.append(f'    "{c_escape(line)}\\n"')
        lines.append(";")
        lines.append("")

    lines.append("#endif /* WAYWAL_SHADERS_EMBEDDED_H */")
    lines.append("")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
