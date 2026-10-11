# Optional profile-guided optimization

CMake exposes `KYTY_PGO_GENERATE` for an instrumented training build and `KYTY_PGO_USE` for a profile supplied by the user. Both are optional. No donor training profile is distributed here.

Use separate training and release directories with matching source and toolchain. Collect nonempty profiles from successful runs, merge them using the matching `llvm-profdata`, and provide the resulting file explicitly to a separate build. Preserve your original game data and prior profiles. A profile from another source version does not prove a benefit or compatible coverage.

`build-windows.cmd` can select a local `_Build/pgo/windows/kyty.profdata` when present. Without a supplied profile it builds without profile use. Instrumented training results must be kept separate from measurements of the final release binary.
