# Launcher design: finding and importing the player's game on every target

Date: 2026-09-17
Status: implemented 2026-09-17 (L0 to L4); see the plan for what was checked where
Parent spec: `2026-09-13-recomp-kit-design.md`

## 1. Goal

Every app the kit builds finds the player's own copy of its game, says
plainly what is wrong when it cannot, and on the targets that cannot play a
folder in place (iPadOS, Android, the web) copies the game into app storage
with progress, resume and a free-space check. The launcher is kit code; a
game repository only supplies data in `game.toml`.

## 2. Decisions

- One launcher per app on native targets (the kit builds one app per game);
  one hub page listing every game on the web.
- Desktop plays an install in place and remembers it; iPadOS, Android and the
  web always import into app storage.
- The launcher appears when the game is not ready or the player asks for it
  (`--launcher`, `RECOMP_LAUNCHER`, Shift or Alt held at start on desktop).
  On a phone or tablet it shows for 1.5 s before a ready game starts, and a
  touch keeps it open. The web hub is always the launcher.
- Saves live in the profile, never next to the game data, so deleting or
  re-importing the game never touches them. The mod overlay already writes
  there.
- Native targets draw the launcher with SDL and the kit's own font; the web
  launcher is an HTML page so importing starts while the `.wasm` downloads.
- A device the host cannot render on (no Vulkan 1.1) still gets the launcher,
  drawn through SDL's window surface: the game imports, and Play explains.
- A folder the player already put in app storage (Finder or Files on iPadOS,
  USB on Android) is moved into place rather than copied, and the rest of it
  removed.

## 3. States

| State | Meaning |
|---|---|
| Not found | No game data where the launcher looks |
| Wrong version | The executable's SHA-256 is not the supported one |
| Incomplete | The executable is right but a `[setup] required_dirs` folder is missing |
| Importing | A copy is running |
| Ready | Everything checks out |

A later launch uses the `.stamp` written by an import (or, for an install
played in place, a cache of the executable's size, mtime and digest) instead
of hashing again.

## 4. Shared core (`host/launcher/`)

- `find_root`: the folder holding the executable, at the picked path or up to
  two levels below it; names compared without case.
- `check`: the states above.
- Sources: a folder, or a ZIP (by path or descriptor), listed as relative
  entries. A ZIP's entries are taken relative to the folder holding the
  executable.
- `import_game`: `[bundle] exclude` rules (as `tools/stage_game_files.py`),
  free space against the copied size plus `[launcher] min_free_mb`, each file
  written as `<name>.part` and renamed, files already complete skipped, the
  stamp removed first and written last, the imported executable hashed before
  the stamp is written. A progress callback returns false to cancel.
- Saved folders: `launcher-folders.txt` in the profile (the old
  `game-path.txt` still resolves).
- Save export and import: the profile as a ZIP, minus the game data and logs.

## 5. `game.toml`

```toml
[launcher]
title = "Majesty Gold HD"                 # default: [game] name
store = "https://www.gog.com/..."         # where to get the game
install_names = ["Majesty Gold HD"]       # folder names detection looks for
gog_ids = ["1207659027"]                  # GOG registry keys (Windows)
steam_ids = []                            # Steam app ids
min_free_mb = 200                         # headroom kept free after an import
```

`tools/gen_game_config.py` emits these, `[setup] required_dirs` and
`[bundle] exclude` as `RECOMP_LAUNCHER_*`, `RECOMP_REQUIRED_DIRS` and
`RECOMP_BUNDLE_EXCLUDE`.

## 6. Targets

| | Game data | Picker | Detection |
|---|---|---|---|
| macOS | in place | folder dialog, drop on window | Wine/CrossOver/Whisky bottles, `~/Games`, volumes |
| Windows | in place | folder dialog, drop | GOG registry, Steam libraries, `C:\GOG Games` |
| Linux | in place (portal paths on Flatpak) | folder dialog | Heroic, Lutris, Wine prefixes, Steam Proton |
| iPadOS | `Documents/game` | Finder/Files copy, folder picker, ZIP | any folder in Documents holding the executable |
| Android | external files `/game` | system folder picker, ZIP, open-with | a folder copied there over USB |
| Web | OPFS `/<game id>/game` | directory picker, folder input, drop, ZIP | an earlier import |

Target notes:

- iPadOS: `Documents/game` wins over the copy bundled into developer builds;
  the bundle is imported only when `Documents/game` is not found. The game folder is
  excluded from iCloud backup; the screen stays awake while importing.
- Android: the copy runs while a foreground service with a progress
  notification keeps the process alive; an import that was stopped continues
  from the files already copied when it is started again. No all-files
  permission.
- Web: the copy runs in a worker with synchronous OPFS handles, asks for
  persistent storage and shows the quota first. Nothing is uploaded.

## 7. Milestones

- L0: core, `game.toml` keys, portable tests, the iPadOS resolver fix.
- L1: the SDL launcher screen on desktop with folder dialog, drop and
  detection.
- L2: Android picker, ZIP and the import service.
- L3: iPadOS document picker, ZIP, backup exclusion.
- L4: the web hub page and OPFS import.

Each milestone fast-forwards kit `main`; game repositories re-pin at
milestone boundaries.
