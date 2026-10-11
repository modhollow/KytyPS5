# Optional shader and pipeline preparation

The standalone `kyty_shader_precompile` target and the Python helpers in this directory collect shader and pipeline inputs from a user-supplied game and can prepare compatible cache entries before play. Build the native tool with `build-windows.cmd kyty_shader_precompile`. The root `precompile-windows.ps1` script coordinates preparation; inspect its arguments before running it.

`pass-states.json` provides pipeline-state metadata. The native collector accepts an explicit `--states` file. The tracked versioned `.hints` files are metadata records containing word counts, hashes and pipeline descriptions, with no shader executable code or resource-buffer snapshots. The generator and reader source remain available. Generated seeds, recordings and driver/static caches are local outputs and must remain separate from distributed source.

Keep game version, source/producer, instrumentation, device and driver identities intact. Do not retag outputs or assume a cache from another build is compatible. Full preparation is optional; it does not prove complete coverage or eliminate every first-use stall. Normal boot and tested limits are documented in the [installation guide](https://github.com/modhollow/KytyPS5/wiki/Installation-and-Boot) and [testing guide](https://github.com/modhollow/KytyPS5/wiki/Testing-and-Known-Issues).

The Python collectors and analysis commands remain in `precompile.py`, `keys.py` and neighboring modules. They require their own input data and dependencies; consult the command-line parser and source for exact options.
