// Kernel profiles: how each flashcart kernel or loader menu names, places and
// sizes its save files. Several kernels can share one SD card (a DSpico often
// carries Pico Launcher and TWiLight Menu++), so every installed kernel is
// detected and each one's rules are applied to every game.
//
// Rules come from each project's source code (see docs/kernels.md).

#ifndef DSAVE_KERNEL_H
#define DSAVE_KERNEL_H

#include <stdbool.h>
#include <stdint.h>

#include "rom.h"
#include "util.h"

typedef enum {
    KERNEL_PICO,       // Pico Launcher / Pico Loader (DSpico and others)
    KERNEL_TWILIGHT,   // TWiLight Menu++ with nds-bootstrap
    KERNEL_AKMENUNEXT, // akmenu-next
    KERNEL_WOOD,       // Wood R4 / R4iLS
    KERNEL_YSMENU,     // YSMenu
    KERNEL_AKAIO,      // AKAIO
    KERNEL_GENERIC,    // Anything else: "<rom>.sav" beside the ROM
    KERNEL_COUNT
} KernelId;

typedef uint8_t KernelMask; // Bit (1 << KernelId)

#define KERNEL_BIT(id) ((KernelMask)(1u << (id)))

// Where TWiLight Menu++ keeps saves (settings.ini [SRLOADER] SAVE_LOCATION).
typedef enum {
    TWL_SAVES_FOLDER = 0, // <rom dir>/saves/
    TWL_GAMES_FOLDER = 1, // beside the ROM
    TWL_TWLM_FOLDER = 2,  // <root>/_nds/TWiLightMenu/saves/
} TwlSaveLocation;

typedef struct {
    bool installed;
    // TWiLight Menu++
    TwlSaveLocation twl_location;
    // akmenu family (akmenu-next, Wood, AKAIO): globalsettings.ini [system]
    bool nds_sav_ext; // saveext = ".nds.sav" instead of ".sav"
    bool saves_dir;   // savedir = 1 (akmenu-next): "<rom dir>/saves/"
} KernelConfig;

// Pico Loader's save list (_pico/savelist.bin), used to know a game's real
// chip size. Pico and TWiLight both pad save files to 512 KB when a game is
// not listed, so the file size alone is not the chip size.
typedef struct {
    uint32_t game_code;
    uint8_t save_type; // 0 none, 1 EEPROM, 2 FLASH, 3 NAND
    uint8_t log2_size; // 0 = no save
    uint8_t reserved[2];
} SaveListEntry;

typedef struct {
    char root[DSAVE_PATH_MAX];
    KernelConfig cfg[KERNEL_COUNT];
    KernelMask installed;
    KernelId active; // The kernel the user launches games with
    SaveListEntry *savelist;
    int savelist_count;
} KernelSet;

const char *kernel_name(KernelId id);

// Short, stable id used in DSavE's settings.ini ("pico", "twilight", ...).
const char *kernel_key(KernelId id);
bool kernel_from_key(const char *key, KernelId *out);

// True when the profile has been checked on real hardware.
bool kernel_hw_tested(KernelId id);

// Detect installed kernels under root and read their settings. The active
// kernel is taken from _DSavE/settings.ini if valid, else the first
// installed one in KernelId order.
void kernels_detect(KernelSet *ks, const char *root);

void kernels_free(KernelSet *ks);

// Make id the active kernel and remember it in _DSavE/settings.ini.
bool kernels_set_active(KernelSet *ks, KernelId id);

// Step to the next or previous installed kernel.
KernelId kernels_cycle(const KernelSet *ks, int direction);

// TWiLight's per-game save number (0-9) for this ROM, from
// _nds/TWiLightMenu/gamesettings/<rom file name>.ini. 0 when unset.
int kernel_twl_save_number(const KernelSet *ks, const char *rom_path);

// Path of the per-game TWiLight settings file for this ROM.
bool kernel_twl_gamesettings_path(const KernelSet *ks, const char *rom_path,
                                  char *out, size_t size);

// The path kernel id uses for this ROM's save in the given slot. Slot 0 is
// the normal save; TWiLight supports 1-9 (".sav1") and akmenu-next 1-9
// (".1.sav"). Returns false if the kernel has no such slot.
bool kernel_save_path(const KernelSet *ks, KernelId id, const char *rom_path,
                      int slot, char *out, size_t size);

// The slot the kernel will load for this ROM right now.
int kernel_current_slot(const KernelSet *ks, KernelId id, const char *rom_path);

// Highest slot number the kernel supports (0 = no slots).
int kernel_max_slot(KernelId id);

// How a kernel lays out a DSiWare title's save files (see rom.h, DsiPart).
typedef enum {
    DSI_LAYOUT_NONE,    // The kernel doesn't run DSiWare
    DSI_LAYOUT_SIDECAR, // "<rom name>.pub", ".prv" and ".bnr": Pico Loader, and
                        // TWiLight Menu++ running DSiWare in DSi mode
    DSI_LAYOUT_SAV,     // "<rom name>.sav" holds the public save (the private one
                        // if there is no public), plus ".bnr": TWiLight Menu++
                        // running DSiWare through nds-bootstrap in DS mode
} DsiLayout;

// True for the kernels that run DSiWare: Pico Launcher, TWiLight Menu++, and
// Generic (which follows Pico's naming).
bool kernel_runs_dsiware(KernelId id);

// The path kernel id uses for one save file of a DSiWare ROM, in the same
// folder as its .sav (kernel_save_path). TWiLight's slot N uses ".puN",
// ".prN" and ".bnN". In the SAV layout the public and private parts are the
// same file. Returns false if the kernel has no such file.
bool kernel_dsiware_path(const KernelSet *ks, KernelId id, const char *rom_path, int slot,
                         DsiLayout layout, DsiPart part, char *out, size_t size);

// Real chip size for a game code from Pico's save list, or -1 if unknown.
// 0 means the list says the game has no save.
long kernel_chip_size(const KernelSet *ks, const char *game_code);

// "<root>/_DSavE/settings.ini"
void kernels_settings_path(const char *root, char *out, size_t size);

#endif
