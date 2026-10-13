# DSavE

A save manager for the Nintendo DS family that runs on the console itself, in
the spirit of Checkpoint and JKSM. Back up, restore, name and export your game
saves without a PC.

> **Status: early.** This is the save engine with a simple text front end. The
> dual-screen tile UI, compression and physical-cartridge saves are still to
> come (see [Roadmap](#roadmap)). It has not been tested on hardware yet, so
> keep your own copy of anything precious.

## What it does

- **Finds your games.** Scans the SD card for `.nds` files and pairs each with
  its save, whichever loader you use:

  | Loader | Detected by | Save location |
  |---|---|---|
  | TWiLight Menu++ / nds-bootstrap | `_nds/TWiLightMenu/` | `<rom folder>/saves/<rom>.sav` |
  | Wood R4 | `__rpg/` | `<rom>.sav` beside the ROM |
  | YSMenu | `TTMenu/` | `<rom>.sav` beside the ROM |
  | AKAIO | `__aio/` | `<rom>.sav` beside the ROM |
  | Anything else | | `<rom>.sav` beside the ROM |

  Every layout is checked for every game, so a save left behind by another
  loader is still found.

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
     game code and size must match the game. A corrupt, truncated or
     wrong-game backup is refused before anything is written.
  2. The current save is backed up automatically (`auto_<timestamp>`).
  3. The save is overwritten in place, so flashcarts that need a contiguous,
     pre-allocated save file (Wood R4) keep working.
  4. The result is read back and compared. If it does not match, the old save
     is put back.

- **Backs up safely.** Each backup is written to a hidden temp folder,
  verified, and only then renamed into place. A blank (all `00`/`FF`) save is
  flagged rather than silently kept.

## Controls

| Screen | Button | Action |
|---|---|---|
| Games | Up/Down, Left/Right | Move, page |
| | A | Open the game's backups |
| | X | Quick backup (timestamp name) |
| | START | Rescan the SD card |
| Backups | A on *New backup* | Back up now |
| | Y on *New backup* | Back up with a name |
| | A | Restore (hold A to confirm) |
| | Y | Rename |
| | X | Move to trash (hold A to confirm) |
| | SELECT | Export to `_DSavE/export/` |
| | B | Back to games |
| Name editor | Up/Down, Left/Right, X | Change letter, move, delete letter |
| | A / B | Save / cancel |

## Building

DSavE builds with [BlocksDS](https://blocksds.skylyrac.net). With Docker:

```sh
docker run --rm -v "$PWD":/work -w /work skylyrac/blocksds:slim-latest make
```

This produces `DSavE.nds`. Copy it to your SD card and launch it from your
loader. Every push is also built by GitHub Actions; the `.nds` is attached to
each run.

The engine in `source/core/` is plain C with no DS dependencies, so it is
tested on a PC against a fake SD card:

```sh
make -C tests
```

## Roadmap

1. **Engine** (this release): SD scan, loader detection, backup, restore,
   rename, trash, export.
2. **Compression**: optional zlib-compressed archives alongside the raw
   `.sav`.
3. **Dual-screen UI**: game info and backup history on top, touch list and
   button actions on the bottom.
4. **Release**: hardware testing on DS Lite and DSi, then a tagged release.

Later: physical cartridge saves (DSi with Unlaunch, DS Lite hot-swap), DSiWare
`.pub`/`.prv` saves, and format conversion (`.dsv`, `.duc`, no$gba).

## License

DSavE is licensed under the GNU General Public License v3.0 or later. See
[LICENSE](LICENSE).
