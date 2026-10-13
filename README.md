# DSavE

A save manager for the Nintendo DS family that runs on the console itself, in
the spirit of Checkpoint and JKSM. Back up, restore, name and export your game
saves without a PC.

<p>
<img src="docs/images/main.png" alt="Game list and backup history" width="200">
<img src="docs/images/restore.png" alt="Hold A to restore" width="200">
<img src="docs/images/copies.png" alt="Copies of a save across kernels" width="200">
<img src="docs/images/rename.png" alt="Touch keyboard" width="200">
</p>

> **Status: early.** It has not been tested on hardware yet, so keep your own
> copy of anything precious. Physical-cartridge saves are still to come (see
> [Roadmap](#roadmap)).

## What it does

- **Finds your games and their saves, whichever kernel you use.** DSavE
  detects every kernel on the card and applies each one's save rules,
  including its settings files and save slots:

  | Kernel | Detected by | Default save location |
  |---|---|---|
  | Pico Launcher (DSpico) | `_pico/` | `<rom>.sav` beside the ROM |
  | TWiLight Menu++ | `_nds/TWiLightMenu/` | `<rom folder>/saves/<rom>.sav` |
  | akmenu-next | `_nds/akmenunext/` | `<rom>.sav` beside the ROM |
  | Wood R4 *(untested)* | `__rpg/` | `<rom>.sav` beside the ROM |
  | YSMenu | `TTMenu/` | `<rom>.sav` beside the ROM |
  | AKAIO | `__aio/` | `<rom>.sav` beside the ROM |

  When several kernels share a card, pick the one you launch games with in
  the menu (START). See [docs/kernels.md](docs/kernels.md) for every rule and its
  source.

- **Keeps as many backups as you like**, each with a name (a timestamp by
  default). Backups are raw `.sav` files, so emulators and PC save editors can
  open them directly:

  ```
  _DSavE/saves/<GAMECODE> <title>/<backup name>/<title>.sav
                                               /backup.ini   (size, CRC-32, date)
  _DSavE/export/   exported copies
  _DSavE/_TRASH_/  deleted backups (nothing is ever hard-deleted)
  ```

- **Restores safely.**
  1. The backup's size and CRC-32 are checked against `backup.ini`, and its
     game code must match the game. A corrupt, truncated or wrong-game
     backup is refused before anything is written.
  2. The current save is backed up automatically (`auto_<timestamp>`).
  3. The save is overwritten in place, so flashcarts that need a contiguous,
     pre-allocated save file (Wood R4) keep working. If the kernel padded
     the save (Pico and TWiLight pad to 512 KB), the padding is kept.
  4. The result is read back and compared. If it does not match, the old save
     is put back.

- **Backs up safely.** Each backup is written to a hidden temp folder,
  verified, and only then renamed into place. A blank (all `00`/`FF`) save is
  flagged rather than silently kept.

- **Compresses backups if you want.** Turn on *Compress new backups* in the
  menu, or press X on any backup to compress or decompress it. Compressed
  backups are standard gzip files (`<title>.sav.gz`), so `gunzip` or 7-Zip on
  a PC turns them back into a raw `.sav`. A padded 512 KB save usually shrinks
  to a few KB. Saves over 1 MB stay uncompressed, because the DS Lite has
  only 4 MB of RAM.

## Controls

Everything works with the buttons or the stylus.

**Top screen:** the selected game, its save, and its backup history.
**Bottom screen:** the game list and the action bar.

| Where | Button | Action |
|---|---|---|
| Games | Up/Down, Left/Right | Move, page (or tap and drag the list) |
| | L/R | Choose a backup in the history |
| | A | **Backup** the save now (named by date and time) |
| | B | **Restore** the chosen backup (hold A to confirm) |
| | X | **Compress** or decompress the chosen backup |
| | Y | **Rename/Delete**: rename, move to trash, export a `.sav` for PC, or back up with a name |
| | START | Menu: active kernel, compression, rescan, about |
| | SELECT | Copies of this save across kernels, and TWiLight slots |
| Copies | A on a copy | Copy the current save there (hold A) |
| | A on a slot | Make TWiLight load that slot |
| | START | Copy the current save to every kernel (hold A) |
| | Y | Back up the selected copy |
| | B | Back to the games |
| Keyboard | Tap, or d-pad and A | Type |
| | B, L/R | Delete, shift |
| | START / SELECT | Done / cancel |

## Building

DSavE builds with [BlocksDS](https://blocksds.skylyrac.net). With Docker:

```sh
docker run --rm -v "$PWD":/work -w /work skylyrac/blocksds:slim-latest make
```

This produces `DSavE.nds`. Copy it to your SD card and launch it from your
loader. Every push is also built by GitHub Actions; the `.nds` is attached to
each run.

The engine in `source/core/` and the UI in `source/ui/` are plain C with no
DS dependencies, so they are tested on a PC against fake SD cards:

```sh
make -C tests
```

The UI test presses buttons and taps the screen like a player would and
saves a PNG of both screens after each step in `tests/screenshots/`. CI
uploads them with each run.

The UI fonts are generated from DejaVu Sans by `tools/make_font.py` (needs
Pillow) into `source/ui/font_data.c`, which is committed.

## Roadmap

1. **Kernel profiles**: Pico Launcher, TWiLight Menu++,
   akmenu-next, Wood R4, YSMenu and AKAIO save rules, plus backup, restore,
   rename, trash and export.
2. **Cross-kernel tools**: spot out-of-sync copies of a save
   when several kernels share a card, copy a save into another kernel's
   layout, and switch TWiLight save slots.
3. **Dual-screen UI** (this release): game info and backup history on top,
   a touch list and button actions on the bottom, plus optional compressed
   backups.
4. **Release**: hardware testing on DSpico and Ace3DS X, then a tagged
   release.

Later: physical cartridge saves (DSi with Unlaunch, DS Lite hot-swap),
DSiWare `.pub`/`.prv` saves, and format conversion (`.dsv`, `.duc`, no$gba).

## License

DSavE is licensed under the GNU General Public License v3.0 or later. See
[LICENSE](LICENSE).

It includes [miniz](https://github.com/richgel999/miniz) (MIT, see
`source/third_party/miniz/LICENSE`) and glyphs rendered from
[DejaVu Sans](https://dejavu-fonts.github.io/) (Bitstream Vera license, see
`source/ui/fonts/LICENSE-DejaVu.txt`).
