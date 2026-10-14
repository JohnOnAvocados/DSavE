# Kernel save rules

DSavE detects every kernel installed on the SD card and applies each one's
save rules to every game. These rules were read from each project's source
code. Where a kernel is closed source, the rules come from its open-source
ancestor and are marked as such.

| Kernel | Detected by | Save path | Slots | Settings DSavE reads | Hardware tested |
|---|---|---|---|---|---|
| Pico Launcher / Pico Loader | `_pico/` | `<rom dir>/<rom name>.sav` | none | `_pico/savelist.bin` (chip sizes) | not yet |
| TWiLight Menu++ | `_nds/TWiLightMenu/` | `<rom dir>/saves/<rom name>.sav` (default) | `.sav1`–`.sav9` | `settings.ini`, `gamesettings/<rom>.ini` | not yet |
| akmenu-next | `_nds/akmenunext/` | `<rom dir>/<rom name>.sav` | `.1.sav`–`.9.sav` | `globalsettings.ini` | not yet |
| Wood R4 | `__rpg/` | `<rom dir>/<rom name>.sav` | none | `globalsettings.ini` | no hardware available |
| YSMenu | `TTMenu/` | `<rom dir>/<rom name>.sav` | none | none | not yet |
| AKAIO | `__aio/` | `<rom dir>/<rom name>.sav` | none | `globalsettings.ini` | not yet |
| Generic | (nothing else found) | `<rom dir>/<rom name>.sav` | none | none | n/a |

`<rom name>` is the ROM's file name without its extension. DSiWare titles
keep their saves differently; see [DSiWare saves](#dsiware-saves).

## Pico Launcher / Pico Loader

Source: [LNH-team/pico-loader](https://github.com/LNH-team/pico-loader) and
[pico-launcher](https://github.com/LNH-team/pico-launcher).

- The launcher passes an empty save path. The loader then replaces the ROM's
  extension with `.sav` (`arm7/source/main.cpp`, `handleSavePath`).
- `CardSaveArranger` sizes the save from `_pico/savelist.bin`, a list of
  8-byte entries `{u32 gameCode, u8 saveType, u8 log2(size), u8 reserved[2]}`.
  Games that aren't listed get 512 KB. The file is padded with `0xFF` up to
  that size and is never shrunk.
- DSavE reads the same list to know each game's real chip size. It uses that
  to trim exports and to show how much of the file is padding.

## TWiLight Menu++ (nds-bootstrap)

Source: [DS-Homebrew/TWiLightMenu](https://github.com/DS-Homebrew/TWiLightMenu).

- `_nds/TWiLightMenu/settings.ini`, `[SRLOADER] SAVE_LOCATION`:
  `0` = `<rom dir>/saves/` (default), `1` = beside the ROM,
  `2` = `_nds/TWiLightMenu/saves/`.
- Per-game save number: `_nds/TWiLightMenu/gamesettings/<rom file name>.ini`,
  `[GAMESETTINGS] SAVE_NUMBER` 0–9. A value of N makes the save `<rom name>.savN`.
  The setting is ignored when `SAVE_LOCATION = 1`.
- When TWiLight launches a game through a flashcart's own kernel, it **moves**
  `saves/<rom name>.sav` to sit beside the ROM. The same game's save can
  therefore end up in either place.
- The save is created or expanded to its database size (512 KB by default),
  filled with `0xFF`, and never shrunk.

## akmenu-next, Wood R4, AKAIO

Source: [coderkei/akmenu-next](https://github.com/coderkei/akmenu-next).
Wood R4 and AKAIO are closed-source descendants of akmenu and are assumed to
follow the same naming.

- `globalsettings.ini [system] saveext`: `.sav` (default) drops the ROM's
  3-letter extension, while `.nds.sav` keeps it (`<rom file>.nds.sav`).
- akmenu-next only: `savedir = 1` puts saves in `<rom dir>/saves/`.
- Slots: `<rom name>.<N>.sav` (`cSaveManager::generateSaveName`). akmenu-next
  stores the per-game slot in an undocumented binary file, so DSavE treats
  slot 0 as the current one.
- akmenu-next launches games through nds-bootstrap or Pico Loader, but it
  passes its own save path, so its rules apply when it is the launcher.

## YSMenu

Saves sit beside the ROM as `<rom name>.sav`. YSMenu is known to sometimes
create 8.3 short names such as `SIMS4~1.SAV` for long ROM names. DSavE does
not yet detect those.

## DSiWare saves

A DSiWare title is a `.nds` (or `.dsi`) installed to the DSi's NAND rather
than sold on a card. It has no save chip. On a DSi it saves to up to three
files in its NAND folder; loaders that run DSiWare from an SD card keep
them beside the ROM instead:

| File on a DSi | Beside the ROM | What it is | Size |
|---|---|---|---|
| `public.sav` | `<rom name>.pub` | FAT12 image | header `0x238` (u32) |
| `private.sav` | `<rom name>.prv` | FAT12 image | header `0x23C` (u32) |
| `banner.sav` | `<rom name>.bnr` | the title's own animated icon | `0x4000`, when header `0x1BF` bit 2 is set |

DSavE treats a ROM as DSiWare when its unit code (`0x012`) has bit 1 set
(it runs in DSi mode) and the high word of its title ID (`0x234`) is
`0003xxxx` with bit 2 set: `00030004` for DSiWare, `00030005`, `00030015`
and `00030017` for system titles. DSi-enhanced game cards are `00030000`.
DS-mode DSiWare such as Download Play has unit code 0 and no save.

Sources, read at TWiLightMenu `0eed9d6`, nds-bootstrap `cbc71bc`,
pico-loader `b9f923e` and pico-launcher `dc64a34`:

- Header offsets: TWiLight Menu++'s `sNDSHeaderExt`
  (`romsel_dsimenutheme/arm9/source/ndsheaderbanner.h`: `dsi_flags` at
  `0x1BF`, `dsi_tid2` at `0x234`, `pubSavSize` at `0x238`, `prvSavSize` at
  `0x23C`), Pico Loader's `nds_header_twl_t` (`common/ndsHeader.h`), which
  also has `IsDsiWare()`: title ID bit 2 of the high word, and nds-bootstrap
  (`retail/arm9/source/conf_sd.cpp`, which reads the two sizes at `0x238`).
  TWiLight's own test is unit code non-zero and access control (`0x1B4`)
  bit 4 (`romsel_dsimenutheme/arm9/source/iconTitle.cpp`).

### Pico Loader / Pico Launcher

Pico Launcher lists DSiWare like any ROM and passes no save path. Pico
Loader's `DsiWareSaveArranger::SetupDsiWareSave`
(`arm7/source/loader/DsiWareSaveArranger.cpp`, called from `NdsLoader.cpp`)
replaces the ROM's extension with `.prv`, `.pub` and `.bnr` for each file
the header asks for. A missing or short file is grown to the header's size
(FAT12-formatted, or zero-filled for the banner) and never shrunk. There
are no slots. Pico Loader redirects the rest of NAND to the SD card
(`photo/`, `shared1/`, `shared2/`, `sys/`), but DSavE only manages the
three save files. The title only runs in DSi mode on a DSi or 3DS.

### TWiLight Menu++

`romsel_dsimenutheme/arm9/source/main.cpp` (the DSiWare branch of the
launch code) and `perGameSettings.cpp` (`getPubExtension`,
`getPrvExtension`, `getBnrExtension`):

- The folder follows `SAVE_LOCATION` as for `.sav` files: `<rom
  dir>/saves/`, beside the ROM, or `_nds/TWiLightMenu/saves/`.
- `SAVE_NUMBER` N (not when saves are beside the ROM) makes the files
  `.puN`, `.prN` and `.bnN`.
- Two layouts, chosen at launch. When nds-bootstrap runs the title and
  either the ROM is 64 MB or more, slot-1 mode is on, or the title is on a
  flashcart and TWiLight is in DS mode (B4DS, or no SD access), the title's
  saves are one `<rom name>.sav` (`.savN`) the size of the public save (the
  private one if there is no public), plus the `.bnr`. Otherwise they are
  `.pub`, `.prv` and `.bnr`. nds-bootstrap gets the paths as `SAV_PATH`,
  `PRV_PATH` and `BNR_PATH`.
- Missing files are created at launch: `.pub` and `.prv` FAT12-formatted
  (`createDSiWareSave`), the `.sav` zero-filled, the `.bnr` `0x4000` bytes.
- When TWiLight boots DSiWare through Unlaunch, it first moves `.pub` and
  `.prv` out of `saves/` to sit beside the ROM, and moves them back the next
  time it starts (`title/arm9/source/main.cpp`). When it runs a title from a
  flashcart through the DSi's SD card, it copies the files to
  `sd:/_nds/TWiLightMenu/tempDSiWare.*` and back.

DSavE decides which layout a TWiLight location uses from the files already
there (a `.pub` or `.prv` means the first, a `.sav` the second). With
neither, it assumes the second when DSavE itself runs in DS mode, and the
first in DSi mode.

### What DSavE does

- **Backup:** every file there goes into one backup folder as
  `public.sav`, `private.sav` and `banner.sav`, and `backup.ini` records
  `dsiware=1` and `public_size`, `public_crc32` (and the same for
  `private` and `banner`). `size` and `crc32` describe the main file, the
  public save. DSiWare backups are never compressed.
- **Restore:** each file the title uses and the backup holds is written in
  place, at exactly the backup file's size, never padded. A file shorter
  than the header's size is refused. A missing file is created. The SAV
  layout gets the public save in its `.sav` and no private save. All the
  files there are backed up first as one `auto_` backup, and if any file
  fails to read back, all of them are put back and new ones removed.
- A one-file backup of a DSiWare title (an imported `.sav`, or one DSavE
  0.5 made from TWiLight's `.sav`) is its public save.
- **Copies:** copies are compared over all their files, and a copy writes
  all of them.
- **Export:** `<rom name>.pub`, `.prv`, `.bnr` in `_DSavE/export/`. No `.dsv`.
- **Import:** a `.pub`, `.prv` or `.bnr` in `_DSavE/import/` brings its
  siblings of the same name. A raw `.sav` is taken as the public save. Each
  must be exactly the header's size.
- DSavE never reads or writes the DSi's system NAND.

## Several kernels on one card

A DSpico usually carries Pico Launcher and TWiLight together, and an Ace3DS X
might carry YSMenu, akmenu-next and TWiLight. For each game, DSavE lists every
installed kernel's save path. Kernels that share a path share one entry. The
**active kernel** (chosen in the menu, START, and remembered in
`_DSavE/settings.ini`) decides which save DSavE works on:

1. the active kernel's save, if it exists;
2. otherwise the most recently modified save of any kernel, flagged on screen;
3. otherwise the active kernel's path, where a restore will create the file.

## Comparing and copying between kernels

The **Copies** screen (SELECT on the game list) lists every kernel's copy of
the selected game's save:

- ★ is the current save (see above), ✓ matches it, ≠ differs, and – means
  there is no file yet. The top screen shows the selected copy's path, size,
  CRC and whether it is the most recently played one.
- Copies are compared by their data with trailing `0xFF` padding ignored. A
  64 KB exact save and the same data padded to 512 KB count as the same.
- The newest copy is marked. Remember that TWiLight moves saves out of
  `saves/` when it launches through a flashcart kernel.
- **A** copies the current save to the selected kernel's location. **START**
  copies it to every location, creating missing files, so the game finds the
  same save whichever kernel launches it. Every file that gets overwritten is
  first backed up as `auto_<timestamp>`.
- **Y** backs up the selected copy instead of the current save, so you can
  keep a copy before overwriting it with another kernel's.

### TWiLight save slots

When TWiLight is installed and does not keep saves beside the ROM, the
Copies screen also lists slots 0–9 (`.sav`, `.sav1`…`.sav9`). **A** on a
slot writes `SAVE_NUMBER` to the game's
`_nds/TWiLightMenu/gamesettings/<rom>.ini`, which is exactly what TWiLight's
own per-game settings menu does. DSavE then works on that slot's file.

## Restoring onto padded saves

Because kernels pad saves, a backup and the live file can differ in size:

- **Backup smaller than the save:** the backup is written first and the rest
  is filled with `0xFF`. The file keeps its size.
- **Backup larger than the save:** if the extra part is all `0xFF`, only the
  save's length is written. Otherwise the file grows, so no data is lost.
- **No save yet:** a file of exactly the backup's size is created.

Existing files are always overwritten in place, never deleted and recreated,
so a kernel's pre-allocated, contiguous file (which Wood R4 needs) keeps its
clusters.

## Hardware test checklist

For each kernel you can test (Pico Launcher on a DSpico; TWiLight, YSMenu and
akmenu-next on an Ace3DS X):

1. Play a game until it has a save, then quit.
2. In DSavE, check that the kernel is detected (top screen) and the game shows
   a save. Check the path matches the table above.
3. Make a backup, play on and save again, then restore the backup. Boot the
   game and confirm the old save loads.
4. Restore onto a game that has no save yet. Boot it and confirm the kernel
   accepts the created file.
5. For TWiLight: set a save number in the game's settings and check that DSavE
   follows it.
6. Try the touch screen: tap a game, drag the list, tap the action buttons,
   and rename a backup with the keyboard.
7. Turn on *Compress new backups*, make a backup, restore it, and check the
   game loads. Copy the `.sav.gz` to a PC and check `gunzip` opens it.
8. Note anything that differs and report it so the profile can be fixed and
   marked as tested.

### Game cards (needs a DSi, or a 3DS with TWiLight Menu++)

Start DSavE from TWiLight Menu++ on the console's SD card (not from a
flashcart), with a retail game in slot 1.

1. The card should be the first row in the list, with its chip type on the top
   screen (for example "FLASH 512 KB + IR" for Pokemon HeartGold). If it says
   "No game card" or "Couldn't read", note the game and what it says.
2. Press A to back it up. Then play the game, save, and restore the backup with
   B. Boot the card and check the old save loads.
3. Try a small-save game too (most early games use a 512 B or 8 KB EEPROM), and
   an infrared one (Pokemon HG/SS or B/W) if you have one.
4. Take the card out with DSavE open and press A: it should say the card was
   removed. Put it back and press A to check it again.
5. Report the game code and the chip type DSavE shows for any card that fails,
   so the chip table can be fixed. Back up anything precious with GodMode9i
   first until this has been tested.

### DSiWare

Back up any DSiWare save you care about to a PC first. These need a DSiWare
title with a save (Flipnote Studio, the Art Style series, WarioWare:
Snapped!...). Note the title, its game code and the "Save files" line DSavE
shows.

On the DS Pico and the Ace3DS X in DS mode (DS or DS Lite):

1. **TWiLight Menu++ (DS mode):** launch the title from TWiLight and save.
   In DSavE (TWiLight active), check the save is `saves/<rom>.sav` with a
   `.bnr` if the title has a banner save, and that the size is the title's
   public save size. Back up, play on, restore, and check the old save
   loads.
2. With a TWiLight save number set for the title, check DSavE follows
   `.savN` and `.bnN`.
3. **Pico Launcher:** see whether the title starts at all on a DS Lite (it
   should not, as DSi-mode titles need a DSi). Whether it does or not, note
   which of `.pub`, `.prv`, `.bnr` appear beside the ROM, and check DSavE
   lists them for Pico Launcher.
4. Restore onto a title with no save files yet, then launch it and check the
   loader accepts the files DSavE made.
5. Export a backup and check `_DSavE/export/` holds `<rom>.pub` and the
   others; import them back from `_DSavE/import/`.

Only a DSi or 3DS can check the rest:

6. **TWiLight Menu++ in DSi mode from the SD card:** check the save is
   `.pub`/`.prv`/`.bnr` in `saves/` (or `.puN`... with a save number), back
   up, restore, and check the title loads the old save. If the title
   changes its banner save (its icon), check the restored one comes back.
7. **Pico Launcher on a DSi or 3DS (DSpico):** back up, restore and load
   with the files beside the ROM.
8. **A DSi-mode flashcart with TWiLight** (files copied to
   `sd:/_nds/TWiLightMenu/tempDSiWare.*`): check DSavE sees the files on the
   flashcart after TWiLight has copied them back.
9. **Unlaunch as TWiLight's DSiWare booter:** start DSavE from TWiLight (so
   TWiLight first moves `.pub`/`.prv` back into `saves/`), and check DSavE
   finds them there.

### Emulator saves

1. Copy a save from melonDS (`.sav`) or DeSmuME/DraStic (`.dsv`) into
   `_DSavE/import/`, named like the game's ROM. Open *Import a save* under Y
   on any game: the bottom line should name the ROM's game. Import it, restore
   it, and check the game loads the emulator's progress.
2. If you have no$gba or an Action Replay `.duc`, try one of those too. no$gba
   packs its saves; if DSavE says the file is damaged, please share the file
   so the unpacking can be checked.
3. Under Y, use *Export .sav* and *Export .dsv*, copy the files from
   `_DSavE/export/` to a PC, and load each in its emulator.
