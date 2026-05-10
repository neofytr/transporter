# doctest (vendored)

- Upstream: https://github.com/doctest/doctest
- Tag: v2.5.2
- Commit: 6804767ee637789db8a5cb281381cae98dc36906
- Imported: 2026-05-10
- License: MIT (see `LICENSE`).

Single-header drop. Used by unit tests that opt in via `doctest_dep`. The
existing per-binary `main()` tests stay as-is; new tests pick doctest by
defining `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` and including `doctest.h`.

Build glue lives in `third_party/doctest/meson.build`.
