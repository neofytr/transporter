# libalac (vendored)

- Upstream: https://github.com/macosforge/alac
- Commit: c38887c5c5e64a4b31108733bd79ca9b2496d987
- Imported: 2026-05-10
- License: Apache-2.0 (see `LICENSE`).

Imported tree: `codec/` only. The `convert-utility/` directory and the
upstream top-level `makefile` are not vendored — they pull in foundation
helpers and a CLI we do not use.

Files in `codec/` are unmodified. Build glue lives in
`third_party/alac/meson.build` (separate from upstream sources).
