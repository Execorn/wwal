#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate C23/C++23 header strings from standalone GLSL shader sources."""

from __future__ import annotations

import os
import re
import sys
import tempfile


def c_escape(line: str) -> str:
    return line.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t")


def normalize_shader_lines(raw: str, path: str) -> list[str]:
    text = raw.lstrip("\ufeff").replace("\r\n", "\n").replace("\r", "\n")
    filtered: list[str] = []
    seen_code = False

    for line in text.split("\n"):
        stripped = line.strip()
        # Strip SPDX headers and leading blank lines so GLSL #version stays on line 1.
        if stripped.startswith("// SPDX-License-Identifier:"):
            continue
        if not seen_code and stripped == "":
            continue
        seen_code = True
        filtered.append(line)

    while filtered and filtered[-1].strip() == "":
        filtered.pop()

    if not filtered:
        raise ValueError(f"Shader source is empty after stripping headers: {path}")

    for idx, line in enumerate(filtered):
        if line.lstrip().startswith("#version") and idx != 0:
            raise ValueError(
                f"{path}:{idx + 1}: #version directive must appear on line 1 of embedded shader"
            )

    return filtered


def shader_symbol_name(path: str) -> str:
    stem = os.path.splitext(os.path.basename(path))[0].upper()
    if stem == "DISSOLVE_NOISE":
        stem = "NOISE"
    sanitized = re.sub(r"[^A-Za-z0-9_]", "_", stem).upper()
    if not sanitized or sanitized[0].isdigit():
        sanitized = f"_{sanitized}"
    return f"SHADER_SRC_{sanitized}"


def main() -> int:
    if len(sys.argv) < 3:
        sys.stderr.write("Usage: embed_shaders.py <output.h> <shader1.comp> [...]\n")
        return 1

    out_path = sys.argv[1]
    shader_paths = sys.argv[2:]
    seen_vars: dict[str, str] = {}

    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "#ifndef WAYWAL_SHADERS_EMBEDDED_H",
        "#define WAYWAL_SHADERS_EMBEDDED_H",
        "",
    ]

    for path in shader_paths:
        var_name = shader_symbol_name(path)
        if var_name in seen_vars:
            sys.stderr.write(
                f"Error: duplicate shader symbol '{var_name}' from '{seen_vars[var_name]}' and '{path}'\n"
            )
            return 1
        seen_vars[var_name] = path

        try:
            with open(path, "r", encoding="utf-8") as f:
                raw = f.read()
            shader_lines = normalize_shader_lines(raw, path)
        except (OSError, ValueError) as exc:
            sys.stderr.write(f"Error processing shader '{path}': {exc}\n")
            return 1

        # Emit char arrays so shader strings stay in .rodata without pointer relocations.
        lines.append(f"static const char {var_name}[] =")
        for line in shader_lines:
            lines.append(f'    "{c_escape(line)}\\n"')
        lines.append(";")
        lines.append("")

    lines.append("#endif /* WAYWAL_SHADERS_EMBEDDED_H */")
    lines.append("")

    out_dir = os.path.dirname(os.path.abspath(out_path))
    os.makedirs(out_dir, exist_ok=True)
    tmp_path: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=out_dir,
            prefix=".shaders_embedded_",
            suffix=".tmp",
            delete=False,
        ) as tmp:
            tmp_path = tmp.name
            tmp.write("\n".join(lines))
        os.replace(tmp_path, out_path)
    except Exception:
        if tmp_path is not None and os.path.exists(tmp_path):
            os.unlink(tmp_path)
        raise

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
