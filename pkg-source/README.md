# Elf Arsenal — custom home-screen tile PKG sources

This directory holds the source files for the **Elf Arsenal tile PKG**
(`payloads/elf-arsenal-tile.pkg`). The PKG installs as a home-screen
tile that deeplinks to `http://127.0.0.1:6969/` — i.e. the Elf Arsenal
web UI — so users can launch the loader from the PS5 home screen
instead of resending the ELF over `nc`.

Layout:

```
pkg-source/
├── build_sfo.py            ← regenerates sce_sys/param.sfo from param.json
├── elf-arsenal.gp5         ← project file passed to prospero-pub-cmd
├── sce_sys/
│   ├── param.json          ← human-readable PS5 app metadata
│   ├── param.sfo           ← compiled SFO consumed by the PKG installer
│   └── icon0.png           ← 512x512 Elf Arsenal logo (home-screen tile art)
└── IV9999-PSPS69691_00-ELFARSENAL000001.pkg
                            ← canonical pre-built PKG (also at payloads/elf-arsenal-tile.pkg)
```

## Identity

- **contentId** `IV9999-PSPS69691_00-ELFARSENAL000001`
- **titleId** `PSPS69691`
- **titleName** `Elf Arsenal`
- **deeplinkUri** `http://127.0.0.1:6969/`

## Rebuilding

```
# (1) Optional: edit sce_sys/param.json (titleName, version, etc.)
# (2) Regenerate the SFO so it matches the JSON
python3 build_sfo.py

# (3) Build the PKG via the official Sony tool
# (under wine64 on Linux — the tool itself is x86-64 PE32+)
wine /path/to/prospero-pub-cmd.exe img_create \
     --no_progress_bar \
     elf-arsenal.gp5 \
     ../payloads/elf-arsenal-tile.pkg
```

The result drops the new PKG straight at `payloads/elf-arsenal-tile.pkg`,
which is what the loader auto-fetches at boot via
`/api/homebrew/launcher` (URL constant `LAUNCHER_PKG_URL` in
`src/homebrew.c`).

## Distribution flow

Pushed in two places on `git.etawen.dev/soniciso/elf-arsenal`:

1. **`payloads/elf-arsenal-tile.pkg`** at the top of `main` — what the
   in-loader tile auto-installer downloads on every boot
2. As a **release asset** alongside `elf-arsenal.elf` so users can
   sideload it through `Settings → 📦 PKG Installer → Install from URL`
   if the auto-install fails for any reason.

A copy of the pre-built PKG also sits in this directory under the
canonical contentId filename for offline reproducibility.
