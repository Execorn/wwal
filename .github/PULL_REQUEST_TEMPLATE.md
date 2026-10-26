## Summary

<!-- Briefly describe what this change does and why. Link any related issue (e.g., Closes #12). -->

## Verification Checklist

- [ ] Compiles without warnings on `gcc` and `clang` (`meson compile -C build`)
- [ ] All unit, subsystem, E2E, and fuzz smoke tests pass under ASan + UBSan (`meson test -C build --print-errorlogs`)
- [ ] Formatted with `clang-format` and includes `// SPDX-License-Identifier: GPL-3.0-or-later` on new files
- [ ] Updated `docs/man/*.scd`, `README.md`, and shell completions (`contrib/completions/`) if CLI flags or `config.toml` keys changed
