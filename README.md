# KytyPS5 — Demon's Souls

KytyPS5 is an experimental PlayStation 5 emulator. This fork focuses on Demon's Souls, building on [KytyPS5/KytyPS5](https://github.com/KytyPS5/KytyPS5) and [Chen's Demon's Souls integration](https://github.com/chenxiao07/KytyPS5).

## Install and boot

The [installation and boot guide](https://github.com/modhollow/KytyPS5/wiki/Installation-and-Boot) covers the Windows toolchain, emulator build, game-directory layout, launcher and command-line boot. Use your own legally obtained game copy; game files, keys, saves and shader recordings are not distributed here.

1. Clone with dependencies: `git clone --recurse-submodules https://github.com/modhollow/KytyPS5.git`.
2. On Windows, install the C++ workload for Visual Studio 2022, clang-cl, CMake, Ninja, Python 3 and the Vulkan SDK. The graphical launcher additionally requires Qt; see the guide for exact setup.
3. Build the emulator with `build-windows.cmd`. Build the standalone preparation tool separately with `build-windows.cmd kyty_shader_precompile` when needed.
4. Keep the game directory containing `eboot.bin` and its associated files together. From PowerShell in the checkout, run `.\run-windows.ps1 -Game 'C:\Games\DemonsSouls' -Width 1280 -Height 720 -Set 'KYTY_SHADER_WARMUP=record,KYTY_SHADER_PREFETCH=0'`.

For binaries intended for other PCs, configure an explicit instruction-set baseline such as `-DKYTY_MARCH=x86-64-v3` rather than relying on the build machine's native target. This baseline requires AVX2, FMA and BMI2. Actual release requirements and package contents belong in that release's notes.

## Guides and reports

- [Demon's Souls](https://github.com/modhollow/KytyPS5/wiki/Game-Demons-Souls): game version, settings and run history.
- [Testing and known issues](https://github.com/modhollow/KytyPS5/wiki/Testing-and-Known-Issues): verified progress, limitations, available public versions and the reporting template.
- [Sources and credits](https://github.com/modhollow/KytyPS5/wiki/Sources-and-Credits): source repositories, pinned revisions and component notices.
- [Releases](https://github.com/modhollow/KytyPS5/releases): versioned packages and notes.
- [Report a problem](https://github.com/modhollow/KytyPS5/issues): include the exact public version or commit, game version, hardware, settings and reproduction steps.

## Source and licenses

See [source provenance](SOURCE-PROVENANCE.md), the [GPL-2.0 license](LICENSE), component notices in [LICENSES](LICENSES), and the individual dependency licenses. Preserve these notices when redistributing source or binaries. [Development tools](tools/local/README.md) and [optional shader preparation](tools/local/static-precompile/README.md) are documented separately.
