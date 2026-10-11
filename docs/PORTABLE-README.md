# Windows package setup

Extract the complete package into a short, writable folder. Keep the executable, launcher, DLLs, configuration, notices and any included tools together; update by extracting a complete new package into a separate folder. Preserve your original game, saves and caches.

Provide your own game directory containing `eboot.bin` and its associated files. Use the graphical launcher or, when included in the package, run:

```powershell
.\run-windows.ps1 -Game 'C:\Games\DemonsSouls' -Width 1280 -Height 720
```

See [installation and boot](https://github.com/modhollow/KytyPS5/wiki/Installation-and-Boot) for details and [testing and known issues](https://github.com/modhollow/KytyPS5/wiki/Testing-and-Known-Issues) for version-specific progress and limits. Full shader preparation is optional and is not a routine boot requirement. Do not copy cache files between incompatible source, device, driver or producer identities.

Read the accompanying release notes for the package's actual CPU/GPU requirements, included tools and measured game results. This guide does not promise complete shader coverage, stable performance or campaign completion.
