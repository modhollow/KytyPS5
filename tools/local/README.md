# Development tools

These source tools support diagnostics, recording, optional preparation and controlled comparisons. They operate on user-supplied game data and local outputs; those inputs and outputs are not distributed with the source. Review a tool's arguments and input paths before running it, and preserve originals.

- `compile-srt-aot.py` / `compile-srt-aot-windows.py`: compile exported SRT plans; the ABI header is `src/local/SrtAotAbi.h`.
- `extract-game-shaders.py`, `capture-srt-plans.py`, `shader_precompile_audit.py`: extraction, plan export and preparation audits.
- [Static preparation](static-precompile/README.md): seed collection, hint generation and standalone shader/pipeline preparation.
- `game_recording.py`, `recording_hud.py`, `playtest_log.py`: recording and run-log utilities.
- `profile-game.py`, `perf-code-maps.py`, `prof-inline.py` and report scripts: profiling and analysis.
- `stage-candidate.py`, `run-reviewed.py`, `benchmark-build.py`: explicit configurations and binary inputs for separate builds.
- `benchmark-entry.py`, `quick-benchmark-game.py`, `switch-control.py`, `abba-switch.py`, `abba-cpu.py` and `windows/` scripts: comparison helpers. Some launch games or manipulate save copies; inspect their operation before use.
- [Profile-guided optimization](PGO.md): optional training and use of matching profiles.
- `ps5-recon/`: binary-analysis helpers. Set `KYTY_GAME_EBOOT` to the path of your own executable; these tools do not bundle it.

Optional diagnostic and experimental paths are not evidence of a performance improvement. For normal setup and boot use the [installation guide](https://github.com/modhollow/KytyPS5/wiki/Installation-and-Boot); report measured results against an exact source and binary version.
