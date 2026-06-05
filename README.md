# ⚡ Elf Arsenal — the all-in-one utility for PS5

> ⚠️ **HOME-SCREEN TILE AUTO-INSTALL IS OFF BY DEFAULT. ENABLE IT IN SETTINGS → HOMEBREW → "AUTO-UPDATE ELF ARSENAL UI PKG" IF YOU WANT THE TILE INSTALLED.**

> ⚠️ **TROPHY UNLOCKERS ARE NOW OFF BY DEFAULT.**
> Recent versions of kstuff-lite include built-in trophy code that conflicts
> with the trophy-unlocker daemons. To use the trophy unlockers (Unlock Only /
> Unlock + Earn) you must be on **the last version of kstuff without trophy code**.
> If you are on a newer kstuff, leave the trophy toggles off — the built-in
> kstuff trophy support handles it instead. You can re-enable the daemons in
> **Settings → Trophy** if you know your kstuff build supports it.

> ⚠️ **IMPORTANT — THE LATEST BUILD INCLUDES FPKG-GUARD.**
> FPKG-GUARD LOCKS YOUR PS4 CUSA GAME FOLDERS TO PROTECT THEM.
> **IF YOU WANT TO INSTALL, DELETE, OR REINSTALL A GAME YOU MUST UNLOCK FIRST**
> using the **Unlock** button in the PS4 FPKG Protection settings card before proceeding.
> The lock re-applies automatically after 60 seconds.

The successor to Sonic Loader, rebranded and expanded. One ELF you send to
your console's elfldr that wires up cheats, save mgmt, on-screen FTP, a
trophy-unlocker stack, ShadowMountPlus, kstuff-lite, the PS5 Linux
loader, fan control, NP fake sign-in / sign-out, and a full web UI on
`:6969` so all of it is reachable from any device on your network.

If you're coming from Sonic Loader, **no manual migration needed** — see
[Upgrading from Sonic Loader](#upgrading-from-sonic-loader) at the
bottom.

---

## 💙 Thank you for your patience — apology re: unstable build (2026-05-26)

Yesterday's build introduced a regression that caused the game library to
show "Loading…" on the main page and required a manual Refresh to populate.
This was caused by a JavaScript crash in the boot sequence — leftover
references to DOM elements that were removed during the cheats section
cleanup. It slipped through testing and we're sorry for the inconvenience.

The fix is in the latest build. If you're still seeing a blank game grid on
load, just re-send `elf-arsenal.elf` to your console and hard-refresh the
page (`Ctrl+Shift+R` / `⌘+Shift+R`).

Thank you to everyone who reported the issue and kept testing. You make this
project better — we genuinely appreciate the patience and support. ❤️

---

## ⚠️ v1.6.2 — if you ran an early build and are getting KPs

An early release of v1.6.2 shipped with a broken config that can cause
kernel panics on boot. If you are hitting KPs after updating:

1. **Delete `/data/elf-arsenal/`** from the console via FTP or the Files tab
   before the next boot (or from another payload if Elf Arsenal won't load).
2. **Re-send the latest `elf-arsenal.elf`** — the fresh first-boot pass will
   recreate the data dir and config cleanly.

This wipes your saved settings (trophy toggles, FTP credentials, etc.) but
clears whatever corrupt state the early build left behind. Everything else
(cheats, patches, saves) lives outside `/data/elf-arsenal/` and is untouched.

---

## 🚨 Anti-piracy disclaimer

Elf Arsenal is built for **the legitimate-use side of the PS5 homebrew
scene**: legally-owned games, save management, fan translations,
accessibility tools, single-player cheats, and the PS5 Linux loader.

We do not condone piracy, and Elf Arsenal includes **no piracy tooling**:
no PSN spoofing for online play, no PKG signature bypass for unsigned
commercial titles, no anti-cheat evasion for multiplayer. If your goal
is to use Elf Arsenal for piracy, please use something else — and stop
giving the homebrew community grief from Sony.

What we DO support:

- Cheats and patches on **single-player titles you own**
- Save backup, restore, encrypt/decrypt, transfer between profiles
- The PS5 Linux loader (`ps5-linux/ps5-linux-loader`)
- Trophy unlocks on **your own profile, offline only**
- Avatar / profile customization
- App database management (PKG installs, tile management)
- Fan curve / thermal pinning
- Local-network file management via FTP/SMB

---

## 📦 What's inside

The single `elf-arsenal.elf` you send to elfldr contains every sub-payload
below; nothing else needs to be downloaded on first boot.

| Sub-payload | What it does |
|---|---|
| `kstuff-lite` | Userland kernel patches: hijack-check bypass, mount permissions, debug syscalls — boot-time spawned, configurable per-FW. |
| `ShadowMountPlus` (SMP) | Cross-mount installed games from external storage. Embedded icon = Elf Arsenal art. |
| `klogsrv` | Forwards `/dev/klog` to TCP `:3232` so the web UI Klog tab + `nc <ip> 3232` work. |
| `ftpsrv` / `zftpd` | Pick your FTP daemon — bundled `ftpsrv` (default) or `zftpd`. Port `:2121`, anonymous. |
| `nanoDNS` | Local DNS hijack so `api.github.com`, `git.etawen.dev`, `codeload.github.com` resolve even though Sony's DNS blackholes them. |
| `BackPork` | Auto-presses "Don't update" on the system-update nag screen. |
| `garlic-worker` | Background save encrypt/decrypt jobs for [garlicsaves.com](https://garlicsaves.com). Idle-cost ≈ 0. |
| `garlic-savemgr` | Interactive save manager UI on `:8082`. |
| `np-fake-signin` | Forges an offline NP sign-in for the foreground user. Required for trophies on retail. |
| `np-fake-signout` | Undoes np-fake-signin's signin flags **without** erasing username / accountId. Use when trophies stop earning on a profile. |
| `np-restore-account` | Restore registry from a previously-extracted `config.dat`. |
| `trophy-unlocker-all` | Legacy SCC-patch trophy unlocker (FW ≤ 6.50). On by default. |
| `trophy-unlocker-uds` | Universal event-API trophy unlocker (any FW). Off by default — flip on if the legacy one doesn't latch. |
| `lapyjb` | Per-app jailbreak daemon — replaces etaHEN's HijackerCommand for app PID escalation. |
| `dpi` | Direct Package Installer (loopback TCP, port 9040). |
| `ps5-app-dumper` | Decrypts owned-game PKGs into a fakePKG-installable form. |
| `ps5-linux-loader` | Boot Linux on FW ≤ 6.02. Baked in for first-boot install; release picker for upgrades. |
| `websrv` | The web UI itself on `:6969`. |
| `kmon` | Klog scraper that drives the auto-pause / auto-resume of kstuff during launches. |

---

## 🚀 Quick start

1. **Jailbreak your PS5** with whatever entry-chain works for your FW (`Sleirsgoevy`,
   `byepervisor`, `LapyJB`, etc.) and reach `elfldr` listening on `:9021`.
2. **Send the ELF**:

   ```
   nc -q0 <YOUR_PS5_IP> 9021 < elf-arsenal.elf
   ```

   PowerShell equivalent:
   ```
   $bytes = [System.IO.File]::ReadAllBytes("elf-arsenal.elf")
   $tcp = New-Object System.Net.Sockets.TcpClient
   $tcp.Connect("<YOUR_PS5_IP>", 9021)
   $tcp.GetStream().Write($bytes, 0, $bytes.Length)
   $tcp.Close()
   ```

3. **Open the web UI** at `http://<YOUR_PS5_IP>:6969/` on any device. That's the whole launcher — every tab there exposes one of the sub-payloads above. The top-nav **❓ Help** tab documents every Settings section in detail.

You'll get a one-time NP fake sign-in flow if the foreground profile
doesn't have valid offline trophy state yet. Reboot after that, send the
ELF again, and you're set.

---

## 🌐 PS5 DNS setup — required for downloads + community workers

For Elf Arsenal to fetch new payloads (kstuff / SMP / Linux loader release pickers), install Homebrew apps, and run the Garlic Worker against the community save queue, the PS5 needs a DNS server that knows community domains like `git.etawen.dev`. Sony's default DNS does not.

**Settings → Network → Set Up Internet Connection → Custom → DNS Settings → Manual**, then put one of these in **Primary DNS**:

- **`127.0.0.1`** — uses the bundled **nanoDNS** daemon (default ON, listens on loopback). Resolves community hosts to pinned IPs in `/data/nanodns/nanodns.ini`. Works fully offline. Stops working if you turn nanoDNS off in Settings.
- **`62.210.38.117`** — **Nomadic DNS**, a community-run DNS that mirrors the same overrides. Use this if you prefer not to run nanoDNS, or as a fallback.

Without one of these, GitHub-release pickers, the Homebrew installer, and the Garlic Worker queue all fail with DNS errors. Everything else (sideloading, trophies, CheatRunner, SDK rewriter, auto-applied patches) keeps working without DNS.

### ⚠️ Upgrading from an older Elf Arsenal? Turn the cheat engine OFF before using CheatRunner

The bundled cheat engine and CheatRunner now read from the same `/data/cheatrunner/cheats/` tree. If both are enabled at once they'll fight over memory writes on the running game and can crash the title. Fresh installs default the Elf Arsenal cheat engine to **OFF**; upgrades retain whatever the previous setting was.

Open **Settings → 🎮 Cheat engine** and confirm the toggle is **off** before doing anything in CheatRunner. (If you prefer the Elf Arsenal-side engine, just don't open CheatRunner — neither side touches the game until it's enabled.)

### Why the Garlic Worker matters

Garlic Worker is community infrastructure for [garlicsaves.com](https://garlicsaves.com) — it processes save-encrypt / save-decrypt jobs from a shared queue while your console is idle, with near-zero impact on active gameplay. **Default ON**; leaving it enabled means your PS5 helps the wider community decrypt other users' saves and vice-versa. Turning it off doesn't break anything for you, but the queue gets less throughput. Reach the standalone save UI on `http://<YOUR_PS5_IP>:8082/` (the SaveMgr top-nav link).

### Offline pack (no DNS / no internet)

If your PS5 can't or shouldn't reach the internet, build an offline-pack zip on a machine that has internet:

```
tools/build-offline-pack.sh
```

Produces `dist/elf-arsenal-offline-pack-<date>.zip`. FTP-upload the zip to `/data/elf-arsenal-offline-pack.zip` on the console, then open **Settings → 🔌 Offline pack → Extract now**. Files inside the zip extract directly into `/data/` and land where each picker expects them. Flip the **Offline mode** toggle ON to make every picker serve from the local pack instead of GitHub.

Pack contents:

- kstuff — three sources (EchoStretch latest, drakmor latest, kstuff-lowfw v1.0.3)
- ShadowMountPlus latest tagged release
- PS5 Linux loader latest tagged release
- Y2JB autoloader payloads (`elf-arsenal.elf` + `sonic-loader-no-etahen.elf`)
- Home-screen tile pkg (`elf-arsenal-tile.pkg`)
- Community homebrew app zips — default is `OffAct.zip` + `PKGInstall.zip`; edit `tools/build-offline-pack.sh`'s `HB_LIST` to add more from the full set in `src/homebrew.c` (some, like EDuke32, are ~1 GB)
- Three cheat-repo snapshots (etaHEN/PS5_Cheats, GoldHEN/GoldHEN_Cheat_Repository, TeeKay87/HEN-Cheats-Collection)
- `manifest.json` describing every version included

Not in the pack: PKG Zone catalog and HowLongToBeat — both are live web scrapes that need DNS to work.

---

## 🛠 Where things live in the UI

| Tab / panel | Purpose |
|---|---|
| **Home (carousel)** | Tile launcher mirroring `/system_data/priv/mms/app.db`. Hover/click a tile → spotlight with pic0 backdrop + activity stats. |
| **🐧 PS5 Linux** *(FW ≤ 6.02 only)* | One-click "Launch now (boot Linux)" + release picker for upgrades past the bundled `v2.1`. |
| **Klog** | Live `/dev/klog` tail. Filter, pause/resume, clear. |
| **Files** | FTP-style file manager: copy/move/delete across USB, ext1, `/data`, `/system_data`. |
| **Stats** | CPU/SOC temps, fan threshold, loader pid, klog buffer depth. |
| **Settings** | Every toggle the loader has. See below. |

### Settings highlights

- **Background services** — toggle klogsrv, trophy-all, trophy-uds.
- **🪪 NP fake sign-in** — spawns the signin payload, with the trophy disclaimer.
- **🚪 NP fake sign-out** — preserves username + accountId, only flips the signin flags. Use when trophies stop earning.
- **🎮 Cheat engine** — master toggle + cheat repo download (etaHEN PS5_Cheats, GoldHEN, TeeKay87 HEN merged). Auto-apply patches subsection at the bottom drops `.json/.shn/.mc4` files into `/data/elf-arsenal/patches/` and applies them on each game launch.
- **🏆 Trophy unlocker** — toggles for both daemons + step-by-step instructions for unlocking a specific game.
- **🚀 Y2JB autoloader sync** — refresh `elf-arsenal.elf` in every `ps5_autoloader/` folder on every attached USB.
- **🌡 Fan control** — pinned thermal threshold (re-applied every 15 s so the firmware can't undo it on game launch).
- **kstuff-lite picker** — pick any release from EchoStretch or drakmor.
- **ShadowMountPlus picker** — pick any release from drakmor/ShadowMountPlus.
- **PS5 Linux loader picker** *(FW ≤ 6.02)* — pick any release from ps5-linux/ps5-linux-loader.
- **Garlic Worker** + **SaveMgr** — community save infra toggles.
- **Avatar manager** — drop a PNG, get it scaled to PS5 profile dimensions, apply to current user.
- **Files / cheats / patches paths** — drop-zone reference for the FTP server.

---

## 🧩 Rebuild PS4 FPKG DB (survive a Sony "Rebuild Database")

Sony's safe-mode **Rebuild Database** drops PS4 `fpkg` (`CUSA…`) titles from the
application database — they vanish from the home screen even though the game
data is still on disk. Elf Arsenal can put them back, with the correct PSN
concept, art and launch metadata, so they **relaunch properly** — fully offline
(no PSN, no network).

Find it in **Settings → "Rebuild PS4 FPKG DB"**.

### The one hard requirement: don't keep fpkg on internal storage

A rebuild **erases internal** (`/user/app`) fpkg data — that is unrecoverable.
Data on **extended storage survives**, so move any fpkg game you care about to:

- **M.2** (NVMe extended storage) — **always survives** a rebuild. Recommended.
- **USB** extended storage — survives **only if the drive is UNPLUGGED during
  the rebuild**. Unplug it before you start the rebuild, then plug it back in
  once the console has booted. (USB titles are then re-registered by the system
  itself; this tool mainly restores the M.2 ones.)

Move games via **Settings → Storage** on the PS5.

### How the backups work

Elf Arsenal keeps verbatim snapshots of each fpkg title's launch registration in
`/data/FPKGDBBCKUP/` (on `/data`, which a rebuild does **not** touch). This is
necessary because one field — the system-computed installed size — can't be
reproduced from the package alone, so a verbatim row is what makes the title
launch rather than show a "view product" store page.

- **`current/`** — the latest snapshot. A lightweight daemon watches the app
  database and updates this automatically after any install / move / SMP add.
- **`previous/`** — the last working snapshot from **before your most recent
  install**. On every new install the daemon copies `current → previous` *before*
  folding the new game in, so `previous` is always exactly one install behind. If
  a fresh install corrupts the database, restore from `previous` to roll back
  that one install cleanly.

A **💾 Backup now** button (in the Metadata card) forces an immediate snapshot —
handy right before you trigger a rebuild, though the daemon already keeps
`current` up to date.

### Restoring after a rebuild

1. Make sure your fpkg games were on **M.2** (or USB, unplugged during the
   rebuild as above) **before** you rebuilt.
2. Open **Settings → Rebuild PS4 FPKG DB**.
3. Pick the backup set: **current** (normal) or **previous** (one-install
   rollback if a bad install corrupted things).
4. Hit **⟳ Rebuild FPKG DB**. The listings are restored and the home screen
   refreshes; the games launch from extended storage.

Rebuild always re-registers internal and M.2 titles **from their real on-disk
location** (`param.sfo` + art + Sony's offline concept map
`/system/priv/mms_ro/concept_title.db`), so the row always matches where the data
actually is. USB titles keep their per-drive id, so those are restored verbatim
from the backup (or left for the system to re-register on replug).

### Moving games between drives (auto-heal)

Moving an fpkg game between internal ⇄ M.2 ⇄ USB used to break it: the `0555`
lock blocks the move's own "delete the old copy + rewrite the registration"
step, so the game ends up **registered at the drive it came from** while the data
sits on the new drive — the console then reports *"cannot start, corrupted
data."* The data was always fine; only the registration was stale.

This is now healed automatically. About 15 s after a move finishes, the daemon:

1. **re-points the registration** to the drive the game is actually on (re-synth
   for internal/M.2), and
2. **sweeps the orphaned leftover copy** off the old drive (it unlocks then
   deletes the stale folder), which also stops the system re-importing it as a
   phantom "external" app on the next mount.

You can also trigger the same heal manually any time with **⟳ Rebuild FPKG DB**.

> **Folder locking.** The tool sets each game's **`CUSAxxxxx` folder** (under
> `/user/app`, `/mnt/ext0/user/app`, `/mnt/ext1/user/app`) to **`0555`** while
> leaving the content files (`*.pkg`, `*.json`, `*.pbm`, `*.pbm.backup`, `*.xml`)
> at **`0777`**. Deleting a file requires *write* permission on its parent folder,
> so a `0555` folder makes everything inside undeletable — yet the launcher can
> still traverse (`x`) and read (`r`) the `0777` files, so the game runs normally.
> Survives a Sony DB rebuild / GC.
>
> **To move, delete or reinstall a protected game** through the PS5 UI, first hit
> **🔓 Unlock for move/delete** (in the Rebuild card — sets the folders back to
> `0777`); games re-lock automatically after the next move or rebuild. A normal
> *move* doesn't need this — the daemon unlocks during the move and the auto-heal
> cleans up after — but an *uninstall* or *reinstall-over* does.

---

## 🆘 Need help?

Discord: **https://discord.gg/uPnbsnGAZ** — join the `#elf-arsenal` channel.

That's the only support channel. Issues, PRs, and feature requests live
on this repo:
`https://git.etawen.dev/soniciso/elf-arsenal`.

---

## 🔁 Upgrading from Sonic Loader

If your console previously ran Sonic Loader (`sonic-loader.elf`), the
upgrade is fully automatic:

1. The first time you receive a new release through the Sonic Loader
   in-loader updater, the asset still arrives as `sonic-loader.elf`
   (the old updater's filename gate). That's intentional.
2. On the next boot of that ELF, the new build's **one-shot migration
   pass** (`sonic_migrate.c`) scans every attached `/mnt/usb*`,
   `/mnt/ext*`, `/data`, and `/user/data` once. Anywhere it finds
   `sonic-loader.elf`, it renames it to `elf-arsenal.elf`. Anywhere it
   finds `autoload.txt` / `autoboot.txt` / `ps5_autoloader.txt` /
   `boot.txt` containing `sonic-loader.elf`, it patches the line to
   `elf-arsenal.elf`.
3. A marker file at `/data/elf-arsenal/.elf_arsenal_migrate_v1` blocks
   the scan from re-firing on every boot.
4. Next reboot: your `ps5_autoloader/` flow continues to fire, except
   now it's launching `elf-arsenal.elf`. Same console, same data dir, no
   manual intervention.

The runtime data dir stays at `/data/elf-arsenal/` (cheats, patches,
activity log, avatars, config) so none of your existing settings get
orphaned. If you want a clean install instead, delete that dir before
the first boot.

---

## 🧰 Building from source

```
git clone https://git.etawen.dev/soniciso/elf-arsenal
cd elf-arsenal
make PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
```

That gives you `elf-arsenal.elf`. The Makefile drives `prospero-clang`
from the [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk).

---

## 🙌 Credits & greetz

Carrying these straight forward from the Sonic Loader days — none of
this exists without their work, and Elf Arsenal is the same project
under new branding, so the thanks list is the same list. If you're on
it and want a name added/removed/changed, ping the Discord.

Massive thanks to **j0rdy, flat_z, TheFlow, c0w-ar, earthonion, SIStro,
egycnq, abkarino, gezine, Dr.Yenyen, zecoxao, StonedModder,
VoidWhisper, EchoStretch, BestPig, AlAzif, drakmor, hzhreal,
Team-Alua, hammer-83, idlesauce, ntfargo, shahrilnet, null_ptr** —
and the entire PS5 jailbreak scene.

Special shout to **arksama** for the Lapy JB Daemon — the reason
Elf Arsenal is fully self-contained for app jailbreaking and no
longer needs etaHEN — and for the heavy lift on the UDS trophy-unlock
reverse that made `trophy-unlocker-uds.elf` possible.

Project admin: **maj0r** — also the author of CheatRunner
([@callmemaj0r on X](https://x.com/callmemaj0r) ·
[CheatRunner Discord](https://discord.gg/E4g6fEqp46)). CheatRunner ships
bundled in this repo as the web cheat trainer on `:9999`.

Elf Arsenal is built on top of:

- [ps5-payload-dev](https://github.com/ps5-payload-dev) — `elfldr`, `websrv`, `ftpsrv`, `klogsrv` (the foundation everything else sits on)
- [drakmor/shadowMountPlus](https://github.com/drakmor/shadowMountPlus) — game auto-mounting from external storage
- [EchoStretch/kstuff-lite](https://github.com/EchoStretch/kstuff-lite) and [drakmor/kstuff-lite](https://github.com/drakmor/kstuff-lite) — userland kernel patches
- [BestPig/BackPork](https://github.com/BestPig/BackPork) — library sideloading
- [earthonion/garlic-worker](https://git.etawen.dev/earthonion/garlic-worker), [earthonion/garlic-savemgr](https://git.etawen.dev/earthonion/garlic-savemgr) — save processing
- [earthonion/np-fake-signin](https://git.etawen.dev/earthonion/np-fake-signin), [earthonion/np-account-restore](https://git.etawen.dev/earthonion/np-account-restore) — fake-PSN tooling (the `np-fake-signout` companion in this repo is a new Elf Arsenal payload built on the same registry-keys reverse)
- [EchoStretch/ps5-app-dumper](https://github.com/EchoStretch/ps5-app-dumper) — app dumper
- [ps5-payload-dev/offact](https://github.com/ps5-payload-dev/offact) — offline account activation
- [ps5-linux/ps5-linux-loader](https://github.com/ps5-linux/ps5-linux-loader) — the Linux loader baked-in for first-boot install on FW ≤ 6.02
- [GoldHEN](https://github.com/GoldHEN/GoldHEN_Cheat_Repository), [etaHEN](https://github.com/etaHEN/PS5_Cheats), [TeeKay87/HEN-Cheats-Collection](https://github.com/TeeKay87/HEN-Cheats-Collection) — cheat repositories
- [itsPLK/ps5-y2jb-autoloader](https://github.com/itsPLK/ps5-y2jb-autoloader) — autoloader integration
- [seregonwar/zftpd](https://github.com/seregonwar/zftpd) — alternative FTP daemon (bundled, selectable from Settings)

And the Sonic Loader codebase itself — Elf Arsenal is the direct
successor, retaining the same `/data/elf-arsenal/` data dir for
seamless upgrades.

---

## 📜 License

Each sub-payload retains its own upstream license — see the per-source
headers in `src/`. The Elf Arsenal glue code (web UI, updaters,
migration, etc.) is under the same terms as the upstream Sonic Loader
codebase it was forked from (**GPLv3+**).
