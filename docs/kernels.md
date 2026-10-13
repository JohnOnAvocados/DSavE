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

`<rom name>` is the ROM's file name without its extension.

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

## Several kernels on one card

A DSpico usually carries Pico Launcher and TWiLight together, and an Ace3DS X
might carry YSMenu, akmenu-next and TWiLight. For each game, DSavE lists every
installed kernel's save path. Kernels that share a path share one entry. The
**active kernel** (L/R on the game list, remembered in
`_DSavE/settings.ini`) decides which save DSavE works on:

1. the active kernel's save, if it exists;
2. otherwise the most recently modified save of any kernel, flagged on screen;
3. otherwise the active kernel's path, where a restore will create the file.

## Comparing and copying between kernels

The **Copies** screen (Y on the game list) lists every kernel's copy of the
selected game's save:

- `*` is the current save (see above), `=` matches it, `!` differs, and `-`
  means there is no file yet.
- Copies are compared by their data with trailing `0xFF` padding ignored. A
  64 KB exact save and the same data padded to 512 KB count as the same.
- The newest copy is marked. Remember that TWiLight moves saves out of
  `saves/` when it launches through a flashcart kernel.
- **A** copies the current save to the selected kernel's location. **START**
  copies it to every location, creating missing files, so the game finds the
  same save whichever kernel launches it. Every file that gets overwritten is
  first backed up as `auto_<timestamp>`.

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
6. Note anything that differs and report it so the profile can be fixed and
   marked as tested.
