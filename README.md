# GameIsolatorVFS

Give one Steam game install its **own private user-data folders** — saves, configs,
mod lists — while Steam still sees and times the normal game. Nothing is moved,
no junctions, no second Steam entry. A heavily-modded copy lives happily next to a
clean one, each with completely separate `Documents` / `ProgramData`.

It works by redirecting the Win32 file APIs *inside that one game's processes*: any
path under a folder you list gets transparently rewritten to a folder inside the game
install. The launcher and the game are both covered.

Built to isolate a modded *Mount & Blade II: Bannerlord* overhaul install, but it is
game-agnostic.

---

## The problem

Many games hard-code their user data to a shared location:

```
%USERPROFILE%\Documents\<Game>\        <- saves, Configs, LauncherData.xml, MCM settings
%ProgramData%\<Game>\                  <- crash logs, shader cache
```

Two installs of the same game share that one folder. Install a big overhaul in copy B
and it clobbers copy A's configs and fills the save list with saves vanilla can't load.
There is usually **no launch flag** to move this folder, and the obvious workarounds are
all bad: junction/folder-swapping (fragile, has to be redone every launch), a non-Steam
shortcut (playtime/achievements track under the shortcut, not the game), or proxying
`version.dll`/`winmm.dll` (fails — a `System32` copy loads first and shadows the one in
the game folder).

## The approach

One self-contained DLL that replaces the game's `steam_api64.dll`:

* **It forwards every Steamworks call** to the renamed original (`steam_api64_orig.dll`),
  so Steam init, playtime, overlay and achievements are completely unchanged. `steam_api64`
  is loaded early by both the launcher and the game and exists **only** in the game folder
  (no `System32` twin to shadow it), which makes it the ideal thing to replace.
* **On load it hooks the Win32 path-taking file APIs** (`CreateFileW`, `CreateDirectoryW`,
  `FindFirstFileW`, `GetFileAttributesW`, `MoveFileExW`, `CopyFileW`, … 19 in total, plus
  key ANSI variants) via [MinHook](https://github.com/TsudaKageyu/minhook). For every call
  it checks the path against your configured *real → virtual* prefix pairs and rewrites
  matches before the real API sees them. Everything else passes through untouched.

Because the interception is at the actual filesystem call — not at `SHGetFolderPath` — it
does not matter how a program built the path or whether it cached it. The launcher's
`Documents\<Game>` write lands in your virtual folder just like the game's does.

The hooks are installed from a worker thread, **not** from `DllMain` — MinHook suspends
threads to patch, which faults under the loader lock on real Windows. The DLL depends only
on `kernel32` + `msvcrt` (both always already loaded), so there is no dependency for the
loader to chase.

> This is deliberately **one** DLL. Earlier designs split it into a proxy plus a separate
> `vfscore.dll`; the Windows loader failed the dependency load with `0x800701E7`. Folding
> everything into the single `steam_api64.dll` fixed it.

## Works across Steamworks versions

The game resolves `steam_api64` functions by name, and the shipped DLL carries a
comprehensive, recent Steamworks export set forwarding to `steam_api64_orig` — so it works
across versions and games without regenerating anything. If some game ever ships a function
this set lacks, regenerate the forwarder list and rebuild — see [build.sh](build.sh).

---

## Requirements

* 64-bit Windows game that ships `steam_api64.dll` (most modern Steam games).
* **Python 3** (standard library only) to run the installer — or just do the two file
  operations by hand. Nothing is compiled or generated on your machine.
* 32-bit games use `steam_api.dll` and would need a 32-bit build — not shipped here, open
  an issue if you want it.

## Install

1. **Edit `vfsproxy.ini`** (copy it from `vfsproxy.ini.example`). Set the real → virtual
   folder pairs you want redirected — see [Configuration](#configuration).
2. Copy **`vfsproxy.py`**, **`steam_api64.vfs.dll`** and your **`vfsproxy.ini`** into the
   game's binary folder, next to its `steam_api64.dll`. For Bannerlord that is:
   ```
   ...\Mount & Blade II Bannerlord\<YourInstall>\bin\Win64_Shipping_Client\
   ```
3. From that folder, run:
   ```
   python vfsproxy.py
   ```
   This renames the real `steam_api64.dll` → `steam_api64_orig.dll` and drops in the
   self-contained `steam_api64.vfs.dll` as `steam_api64.dll`. (You can do those two file
   operations by hand instead — no Python needed.)
4. Launch the game normally (through Steam, the launcher, or the game exe). Both the
   launcher and the game are now redirected.

Re-running `vfsproxy.py` is safe and idempotent: if `steam_api64_orig.dll` already exists
it just refreshes the proxy. If Steam ever restores the original `steam_api64.dll` (e.g.
after *Verify integrity of game files* or a game update), run it again.

## Configuration

`vfsproxy.ini` lives next to the DLL in the game's bin folder. Pair the paths by number;
use full absolute paths with no trailing slash. Keep comments on their own lines (a path
may legally contain `;`, so inline comments are not stripped from path values).

```ini
[settings]
; create each virtual root at startup (1/0)
create_dirs = 1
; write vfsproxy.log next to this file (1/0; set 0 once happy)
log         = 1

[paths]
Path1Real    = C:\Users\YOU\Documents\Mount and Blade II Bannerlord
Path1Virtual = E:\SteamLibrary\steamapps\common\Mount & Blade II Bannerlord\TalesAge\MyDocs

Path2Real    = C:\ProgramData\Mount and Blade II Bannerlord
Path2Virtual = E:\SteamLibrary\steamapps\common\Mount & Blade II Bannerlord\TalesAge\MyProgramData

; add as many pairs as you like
; Path3Real    = C:\Users\YOU\AppData\Roaming\SomeGame
; Path3Virtual = E:\...\ThisInstall\MyAppData
```

* Only the exact prefixes you list are touched. Steam's own files and the rest of the game
  install pass through unchanged.
* Matching is case-insensitive and `\`/`/`-insensitive, boundary-aware (so `...\Bannerlord`
  does not accidentally match `...\Bannerlord Tools`).
* To carry over existing settings, copy your current `Documents\<Game>\Configs` into the
  `Path1Virtual` folder before first launch. Otherwise the game starts fresh.

## Verify

`vfsproxy.log` (next to the DLL) shows, per process:

```
attach TaleWorlds.MountAndBlade.Launcher.exe: 19 hooks (0 failed) enabled, 2 path pair(s)
  [1] C:\Users\...\Documents\Mount and Blade II Bannerlord  =>  E:\...\TalesAge\MyDocs
h_CreateFileW -> E:\...\TalesAge\MyDocs\...\Configs\LauncherData.xml
```

You want an `attach` line for **both** `...Launcher.exe` and the game exe. Toggle a mod in
the launcher and confirm `LauncherData.xml` updates under your virtual folder and **not**
under the real `Documents`.

## Revert

Delete `steam_api64.dll`, rename `steam_api64_orig.dll` back to `steam_api64.dll`, and
remove `vfsproxy.ini` / `vfsproxy.log`.

---

## Building from source

The repo ships the prebuilt 64-bit `steam_api64.vfs.dll`, so you need no toolchain to use
it. To rebuild (Linux cross-compile, or an msys2 mingw64 shell on Windows):

```sh
./build.sh      # clones MinHook, builds steam_api64.vfs.dll
```

Needs `mingw-w64` (Debian/Ubuntu: `apt install mingw-w64`).

* `vfscore.c` — the redirector: INI parsing, prefix remap, the MinHook installs (on a
  worker thread), and `DllMain`.
* `steam_api64.def` — the Steamworks forwarders (`Name -> steam_api64_orig.Name`).
* `build.sh` — compiles the two into `steam_api64.vfs.dll`; its header has the one-liner to
  regenerate `steam_api64.def` from a different game's `steam_api64.dll`.

## Notes & caveats

* **Not malware.** The DLL hooks file APIs *inside the game's own processes* to remap a few
  paths you configure, and forwards every Steamworks call to the real DLL untouched. It does
  not phone home, modify Steam, or touch anything outside the folders you list. Source is
  here; read it.
* **Antivirus** sometimes flags unsigned DLLs that install API hooks. If the DLL disappears,
  whitelist the folder.
* **Steam → Verify integrity of game files** restores the original `steam_api64.dll`; just
  run `vfsproxy.py` again afterwards.
* **Online / anti-cheat games:** intended for singleplayer / offline modded installs. Do not
  use with kernel-anti-cheat or competitive online games.
* 64-bit only (see Requirements).

## License

MIT — see [LICENSE](LICENSE).
