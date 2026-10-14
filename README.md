# DSavE

A save manager for the Nintendo DS family that runs on the console itself, in
the spirit of Checkpoint and JKSM. Back up, restore, name, organise and export
your game saves without a PC.

<p>
<img src="docs/images/main.png" alt="Game list and backup history" width="200">
<img src="docs/images/restore.png" alt="Hold A to restore" width="200">
<img src="docs/images/copies.png" alt="Copies of a save across kernels" width="200">
<img src="docs/images/rename.png" alt="Touch keyboard" width="200">
</p>

> [!WARNING]
> **DSavE is new and has not finished hardware testing.** Every release is
> tested on a PC against simulated SD cards and save chips, but not yet on
> every flashcart, kernel and console. Copy any save you care about to a PC
> before you use DSavE on it, and read [Using DSavE on real hardware](#using-dsave-on-real-hardware)
> first.

**Contents:** [Features](#what-it-does) ·
[Hardware support](#hardware-support) ·
[Safety](#using-dsave-on-real-hardware) ·
[Installing](#installing) · [Controls](#controls) ·
[Building](#building-from-source) · [Architecture](#architecture) ·
[Contributing](#contributing) · [License](#license)

## What it does

- **Finds your games and their saves, whichever kernel you use.** DSavE
  detects every kernel on the SD card and applies each one's save rules,
  including its settings files and save slots. When several kernels share a
  card, pick the one you launch games with in the menu (START).
- **Keeps as many named backups as you like.** A backup is named with a
  timestamp by default, and you can rename it on the touch keyboard. Backups
  are plain `.sav` files that emulators and PC save editors open directly.
  Give a backup a short note ("before the final boss") with *Add a note*
  under Y, and mark the ones that matter with *Keep this backup*. A kept
  backup is never pruned and can't be moved to the trash until you unmark it.
- **Restores safely.** Every backup is checked (size, CRC-32 and game code)
  before anything is written, the current save is backed up first, and the
  result is read back and rolled back if it does not match.
- **Keeps a log, with undo.** Every backup, restore, copy, import, export and
  trashed backup is written to `_DSavE/log.txt`. *Activity history* in the
  menu lists them, and A on a restore puts back the save it replaced.
- **Checks every backup at once.** *Check all backups* in the menu verifies
  each backup's size and CRC-32, and lists damaged backups, ones without a
  `backup.ini`, unfinished or empty folders, and folders for games no longer
  on the card. It only reads.
- **Exports everything as one .zip.** *Export all to .zip* in the menu writes
  `_DSavE/export/DSavE_<date>.zip` with each game's newest backup named after
  its ROM, ready for an emulator, or (Left/Right) every backup laid out as
  `_DSavE/saves` is, to move a whole library to another card.
- **Shows a backup's bytes.** *View in hex* under Y opens a read-only hex
  viewer. X compares it with the current save, then with each other backup
  in turn: differing bytes are highlighted, the top screen lists the ranges
  that differ, and Left/Right jump between them.
- **Compresses backups if you want.** Compressed backups are standard gzip
  (`.sav.gz`), so `gunzip` or 7-Zip on a PC turns them back into a raw save.
- **Imports and exports emulator saves**: melonDS, DeSmuME, DraStic, no$gba
  and Action Replay formats.
- **Syncs a save across kernels.** See every copy of a game's save on the
  card, spot copies that are out of date, copy one into another kernel's
  layout, and switch TWiLight Menu++ save slots.
- **Backs up and restores retail game cards** on a DSi, or a 3DS running
  TWiLight Menu++ or Unlaunch.
- **Handles DSiWare saves.** A DSiWare title saves to up to three files
  (`public.sav`, `private.sav` and `banner.sav` on a DSi), which loaders on
  an SD card keep beside the ROM as `.pub`, `.prv` and `.bnr`. DSavE backs
  them up together, restores them together, and follows TWiLight Menu++'s
  save slots and save location. See [DSiWare](#dsiware).
- **Never hard-deletes anything.** Deleted and pruned backups go to
  `_DSavE/_TRASH_/`.

### Where DSavE keeps its files

```
_DSavE/saves/<GAMECODE> <title>/<backup name>/<title>.sav     (or .sav.gz)
                                             /backup.ini      size, CRC-32, date
_DSavE/saves/<GAMECODE> <title>/<backup name>/public.sav      DSiWare: each file
                                             /private.sav     the title has
                                             /banner.sav
                                             /backup.ini      each one's size and CRC-32
_DSavE/export/    exported saves (<rom name>.sav or .dsv)
_DSavE/import/    saves to import as backups
_DSavE/_TRASH_/   deleted backups
_DSavE/log.txt    what DSavE did, newest last
```

Games are named by their banner title in English, French, German, Italian,
Spanish or Japanese, whichever comes first that the fonts can draw. A game
whose banner is Japanese only is named by its ROM file name instead.

### How a restore works

1. The backup's size and CRC-32 are checked against `backup.ini`, and its
   game code must match the game. A corrupt, truncated or wrong-game backup
   is refused before anything is written.
2. The current save is backed up automatically (`auto_<timestamp>`). Only the
   last 10 auto backups of each game are kept, and older ones go to
   `_TRASH_`. Change the number, or keep them all, with *Keep auto backups* in
   the menu.
3. The save is overwritten in place, so flashcarts that need a contiguous,
   pre-allocated save file (Wood R4) keep working. If the kernel padded the
   save (Pico and TWiLight pad to 512 KB), the padding is kept.
4. The result is read back and compared. If it does not match, the old save
   is put back.

A new backup is written to a hidden temp folder, verified, and only then
renamed into place. A blank save (all `00` or all `FF`) is flagged rather
than silently kept.

A DSiWare restore works the same way on all of the title's files at once:
every file is checked, the current files are backed up together as one
`auto_` backup, each file is overwritten in place at its exact size (they
are disk images, so they are never padded), and if any file does not read
back right, every file is put back.

### Importing and exporting

Put a save in `_DSavE/import/`, choose the game, then *Import a save* under Y.
The file becomes a backup named `import_<file>`, which you can then restore. A
save named after a ROM on the card (`Mario Kart DS.dsv` for
`Mario Kart DS.nds`) goes into that game, whichever game is selected.

| Format | From |
|---|---|
| `.sav` (raw) | melonDS, flashcarts, Checkpoint, JKSM |
| `.dsv` | DeSmuME, DraStic (the footer is dropped) |
| `.sav` with a no$gba header | no$gba, packed or not |
| `.duc`, `.dss` | Action Replay DS save tools |
| `.sav.gz` | DSavE's own compressed backups |
| `.pub`, `.prv`, `.bnr` | DSiWare saves from Pico Loader or TWiLight Menu++ (the files with the same name come along) |

Under Y, *Export .sav* writes the raw save (melonDS, flashcarts) and
*Export .dsv* adds DeSmuME's footer (DeSmuME, DraStic). The file is named
after the ROM and replaces an earlier export of that game. When the chip size
is known (Pico's save list, or a game card), a kernel's `0xFF` padding is cut
off so the emulator sees the right chip. A DSiWare backup exports as
`<rom>.pub`, `.prv` and `.bnr` instead, and has no `.dsv`.

## Hardware support

### Kernels and loaders

| Kernel | Detected by | Default save location | Hardware tested |
|---|---|---|---|
| Pico Launcher (DSpico) | `_pico/` | `<rom>.sav` beside the ROM | not yet |
| TWiLight Menu++ | `_nds/TWiLightMenu/` | `<rom folder>/saves/<rom>.sav` | not yet |
| akmenu-next | `_nds/akmenunext/` | `<rom>.sav` beside the ROM | not yet |
| Wood R4 | `__rpg/` | `<rom>.sav` beside the ROM | no hardware available |
| YSMenu | `TTMenu/` | `<rom>.sav` beside the ROM | not yet |
| AKAIO | `__aio/` | `<rom>.sav` beside the ROM | not yet |
| Anything else | | `<rom>.sav` beside the ROM | n/a |

The rules were read from each project's source code. See
[docs/kernels.md](docs/kernels.md) for every rule, its source, and the save
slots and settings files DSavE follows.

### Game cards (slot 1)

Game card saves need **DSi mode**: a DSi, or a 3DS running TWiLight Menu++ or
Unlaunch, with DSavE started from the console's own SD card. On a DS or DS
Lite, or when DSavE is started from a flashcart, slot 1 holds the flashcart,
so game cards are not available.

| Save chip | Back up | Restore |
|---|---|---|
| EEPROM 512 B, 8 KB, 64 KB, 128 KB | yes | yes |
| FLASH 256 KB to 1 MB | yes | yes |
| Infrared cards (Pokemon HG/SS, B/W) | yes | yes |
| Band Brothers DX (8 MB) and unknown chips | yes | not yet |
| NAND saves (WarioWare D.I.Y.) | no | no |

The game card path has been tested against a simulated chip only. It has not
yet been run on a DSi or 3DS.

### DSiWare

A DSiWare title (a `.nds` whose DSi header says it is installed to NAND,
title ID `00030004`) keeps its save in up to three files of sizes fixed by
its header. DSavE finds them where each loader puts them:

| Loader | Save files | Slots |
|---|---|---|
| Pico Loader (Pico Launcher) | `<rom>.pub`, `.prv`, `.bnr` beside the ROM | none |
| TWiLight Menu++, DSi mode | `<rom>.pub`, `.prv`, `.bnr` in its save folder | `.puN`, `.prN`, `.bnN` |
| TWiLight Menu++, DS mode (nds-bootstrap B4DS) | `<rom>.sav` (the public save) and `<rom>.bnr` in its save folder | `.savN`, `.bnN` |

TWiLight's save folder follows its *Save location* setting, as for DS games.
Which of TWiLight's layouts a card uses is read from the files already
there; for a title with no save yet, DSavE assumes the layout of the mode it
is running in. The other kernels (akmenu-next, Wood, YSMenu, AKAIO) don't
run DSiWare. DSavE never reads or writes the DSi's own NAND. See
[docs/kernels.md](docs/kernels.md#dsiware-saves) for the rules and their
sources.

DSiWare backups are never compressed. The hex viewer and compare show the
public save (or the private one, for a title without one).

### What has been tested

| Area | Tested how |
|---|---|
| Save engine (backup, restore, compression, import, export, sync, DSiWare) | Automated tests on a PC against simulated SD cards, on every push |
| User interface | Automated tests on a PC that press buttons, tap the screen and save screenshots |
| Game card protocol | Automated tests against a simulated save chip |
| no$gba and Action Replay import | Files built by the tests, not files from the real tools |
| Each kernel on real hardware | In progress; see the [hardware test checklist](docs/kernels.md#hardware-test-checklist) |
| DSiWare saves on real hardware | Not yet; built from the loaders' source code and tested against simulated cards ([checklist](docs/kernels.md#dsiware)) |

If you test DSavE on hardware, please report what worked and what didn't (see
[Contributing](#contributing)) so the tables above can be updated.

## Using DSavE on real hardware

DSavE writes to your SD card and, in DSi mode, to the save chip on a game
card. It is careful about how it does this, but it is free software provided
**without any warranty** (see [LICENSE](LICENSE)). In particular:

- **Back up first.** Before you trust DSavE with a save you care about, copy
  that save to a PC. For a game card, make a copy with a tool you already
  trust (such as GodMode9i) until the game card path has been tested on
  hardware.
- **Keep the power on while it works.** Charge the console, or keep it
  plugged in, before backing up or restoring. Don't turn it off, close the
  lid, or press the power button while a backup, restore, import or copy is
  in progress.
- **Don't remove the SD card or the flashcart while DSavE is running.**
  Removing either during a write can corrupt the save, the file system, or
  both.
- **Removing a game card.** If you swap or remove the game card, press A on
  its row so DSavE checks slot 1 again before you use it. Taking it out in the
  middle of a backup or restore is not supported and can leave the card's
  save half-written. Put the card back and restore the auto backup DSavE made
  before it started.
- **Make sure the SD card has free space.** Each backup is a full copy of
  the save (up to 512 KB for a padded save, unless compressed), and DSavE
  keeps an auto backup before every restore.
- **Check the right kernel is selected** (START) when several kernels share
  one SD card. DSavE writes saves where the selected kernel expects them.
- **Wood R4 needs its save file in one piece.** DSavE overwrites saves in
  place to keep them contiguous, but has not been tested on a Wood R4 card.
  Keep a PC copy of Wood saves.
- **Don't restore a save into a different game.** DSavE refuses backups whose
  game code doesn't match, but files you put in `_DSavE/import/` are trusted
  to be the save of the game you import them into.
- **Some saves stay uncompressed.** Saves over 1 MB are never compressed,
  because the DS Lite has only 4 MB of RAM.

If something goes wrong, nothing DSavE replaced is lost: look in the game's
folder under `_DSavE/saves/` for the `auto_` backup, and in `_DSavE/_TRASH_/`
for anything deleted.

## Installing

1. Download `DSavE.nds` from the
   [latest release](https://github.com/JohnOnAvocados/DSavE/releases).
   Development builds are attached to each
   [CI run](https://github.com/JohnOnAvocados/DSavE/actions) on `main` as the
   *DSavE* artifact.
2. Copy it anywhere on your SD card, for example `_nds/DSavE.nds` or next to
   your ROMs.
3. Launch it from your kernel like any homebrew. Kernels patch DLDI for you.
   For game card saves, launch it from TWiLight Menu++ or Unlaunch on the
   DSi's or 3DS's own SD card.

## Controls

Everything works with the buttons or the stylus.

**Top screen:** the selected game, its icon, its save, and its backup history.
**Bottom screen:** the game list and the action bar.

| Where | Button | Action |
|---|---|---|
| Games | Up/Down, Left/Right | Move, page (or tap and drag the list) |
| | L/R | Choose a backup in the history |
| | A | **Backup** the save now (named by date and time); on an empty game card row, check slot 1 again |
| | B | **Restore** the chosen backup (hold A to confirm) |
| | X | **Compress** or decompress the chosen backup |
| | Y | **Rename/Delete**: rename, add a note, keep, move to trash, export a `.sav` or `.dsv` for emulators, back up with a name, or import a save |
| | START | Menu: active kernel, compression, auto backups to keep, rescan, about |
| | SELECT | Copies of this save across kernels, and TWiLight slots |
| Copies | A on a copy | Copy the current save there (hold A) |
| | A on a slot | Make TWiLight load that slot |
| | START | Copy the current save to every kernel (hold A) |
| | Y | Back up the selected copy |
| | B | Back to the games |
| Keyboard | Tap, or d-pad and A | Type |
| | B, L/R | Delete, shift |
| | START / SELECT | Done / cancel |

## Building from source

DSavE builds with [BlocksDS](https://blocksds.skylyrac.net) (tested with
v1.24.0).

### With Docker (recommended)

From the repository root:

```sh
docker run --rm -v "$PWD":/work -w /work skylyrac/blocksds:slim-latest make
```

In Windows PowerShell, use `-v "${PWD}:/work"` instead. This produces
`DSavE.nds` in the repository root; intermediate files go to `build/`.

### With a native BlocksDS install

Install BlocksDS by following its
[setup guide](https://blocksds.skylyrac.net), make sure the
`BLOCKSDS` environment variable points at it (the guide's default is
`/opt/blocksds/core`), then run:

```sh
make          # build DSavE.nds
make clean    # remove build/ and the ROM
```

### Running the tests

The engine and the UI are plain C with no DS dependencies, so they build and
run on a PC with any C99 compiler (`cc`, GCC or Clang):

```sh
make -C tests
```

This builds with AddressSanitizer and UndefinedBehaviorSanitizer and runs two
suites:

- `test_core` exercises the save engine against fake SD cards and a fake
  game card save chip.
- `test_ui` presses buttons and taps the screen like a player would, and
  saves a PNG of both screens after each step in `tests/screenshots/`.

`make -C tests clean` removes the test binaries and screenshots.

### Fonts

The UI fonts are generated from DejaVu Sans into `source/ui/font_data.c`,
which is committed, so you only need this to change them:

```sh
python3 tools/make_font.py [DejaVuSans.ttf DejaVuSans-Bold.ttf]   # needs Pillow
```

### Continuous integration and releases

Every push and pull request runs the host tests and builds the ROM on GitHub
Actions (`.github/workflows/build.yml`). The ROM and the UI screenshots are
attached to each run.

A release is made by pushing a tag that matches `APP_VERSION` in
`source/ui/app.h`, for example `v0.3.0`, or `v0.3.0-beta1` for a
pre-release. `.github/workflows/release.yml` checks the tag against
`APP_VERSION`, builds the ROM and attaches it to a GitHub release.

## Architecture

DSavE is split so that nearly all of it is portable C that can be tested on
a PC. Only `source/main.c` and `source/platform/` touch the DS hardware.

```
             +-----------------------------------------+
  DS only    | main.c        screens, input, main loop |
             | platform/     slot 1 power and SPI bus  |
             +--------------------+--------------------+
                                  |
             +--------------------v--------------------+
  portable   | ui/     app state, input handling,      |
  C99        |         drawing both screens            |
             +--------------------+--------------------+
                                  |
             +--------------------v--------------------+
             | core/   kernels, games, backups,        |
             |         compression, sync, emulator     |
             |         formats, game card protocol     |
             +-----------------------------------------+
                 |                              |
            SD card (libfat / POSIX)     save chip (via a CardSlot bus)
```

| Path | What it does |
|---|---|
| `source/main.c` | Sets both screens up as 16-bit bitmaps, finds the drive DSavE was launched from (`sd:/` or `fat:/`), feeds buttons and stylus to the UI each frame, and copies what it draws to the screens. |
| `source/platform/card_slot.c` | Slot 1 on the console in DSi mode: card power, the card header, and the save chip's SPI bus (AUXSPI). |
| `source/ui/app.c` | The whole user interface: game list, backup history, menus, the copies screen and the touch keyboard. It takes input and draws to two canvases, with no DS headers. |
| `source/ui/gfx.c`, `font_data.c` | A small 2D renderer (rectangles, circles, antialiased text) for BGR555 bitmaps, and the generated fonts. |
| `source/core/kernel.c` | Kernel profiles: how each kernel names, places, sizes and slots its save files, read from its settings files. |
| `source/core/library.c`, `rom.c` | Finds the ROMs on the card, reads their headers and banners (and the DSi header of DSiWare), and pairs each with its live save for every installed kernel. |
| `source/core/backup.c` | Backups, restores, auto backups and pruning, renames, trash, import and export. Writes go through a temp file, are verified, and are only then moved into place. |
| `source/core/savefile.c` | Reads raw `.sav` and gzip `.sav.gz` saves (using miniz). |
| `source/core/sync.c` | Cross-kernel tools: compares a game's copies, copies a save into another kernel's layout, and manages save slots. |
| `source/core/emusave.c` | Reads and writes emulator formats: `.dsv`, no$gba and Action Replay. |
| `source/core/cardsave.c` | Game card save chips: detection, reading and writing over a pluggable SPI bus, and backup and restore with the same checks as SD saves. |
| `source/core/ini.c`, `util.c` | A small INI reader and writer, paths, CRC-32 and file helpers. |
| `source/third_party/miniz/` | [miniz](https://github.com/richgel999/miniz), for gzip. |
| `tests/` | Host tests, a fake SD card, and a fake game card save chip. |
| `tools/make_font.py` | Generates `source/ui/font_data.c`. |
| `docs/kernels.md` | Every kernel's save rules with sources, and the hardware test checklist. |

## Roadmap

Done: kernel profiles and backup, restore, rename, trash and export (v0.1);
cross-kernel tools and the dual-screen touch UI with compression (v0.2);
import, auto backup pruning, game card saves in DSi mode, the game's icon,
and emulator saves (v0.3); DSiWare saves (v0.6).

Next:

1. Hardware testing on DSpico and Ace3DS X, and a tested 1.0 release.
2. Quality of life: back up every changed save at once, skip duplicate
   backups, mark the backup that is loaded, and sort and filter the list.
3. A trash browser to restore deleted backups.

Later: game cards on a DS Lite (hot-swap), and writing Band Brothers DX's
8 MB chip.

## Contributing

Bug reports and hardware test results are the most useful contributions right
now. When you open an
[issue](https://github.com/JohnOnAvocados/DSavE/issues), please include:

- the console (DS, DS Lite, DSi, 3DS) and the flashcart or loader,
- the kernel and its version,
- the game and its game code (shown on the top screen),
- what you did, what you expected, and what happened.

Pull requests are welcome. Please run `make -C tests` and the ROM build
before opening one; CI runs both.

## License

DSavE is licensed under the GNU General Public License v3.0 or later. See
[LICENSE](LICENSE). It comes with no warranty.

It includes [miniz](https://github.com/richgel999/miniz) (MIT, see
`source/third_party/miniz/LICENSE`) and glyphs rendered from
[DejaVu Sans](https://dejavu-fonts.github.io/) (Bitstream Vera license, see
`source/ui/fonts/LICENSE-DejaVu.txt`). The game card code follows GBATEK and
the save code of GodMode9i and Pokedoc's savegame_manager.

Nintendo DS, DS Lite and DSi are trademarks of Nintendo. DSavE is not
affiliated with or endorsed by Nintendo.
