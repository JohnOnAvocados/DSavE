// Saves on a retail game card in slot 1: finding the card's save chip,
// reading and writing it over the card's SPI bus (AUXSPI), and backing it up
// and restoring it with the same folders, checks and auto backups as saves
// on the SD card.
//
// Slot 1 is only free when DSavE runs in DSi mode from the console's SD card
// (DSi, or 3DS with TWiLight Menu++ or Unlaunch). On a DS Lite, or from a
// flashcart, the flashcart is the card in slot 1.
//
// The chip protocol follows GBATEK ("DS Cartridge Backup") and the save code
// of GodMode9i and Pokedoc's savegame_manager (GPL-2.0-or-later). The bus is
// supplied by the caller: the DS hardware in source/platform/card_slot.c, or a fake
// chip in the tests, so this file is plain C.

#ifndef DSAVE_CARDSAVE_H
#define DSAVE_CARDSAVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "backup.h"
#include "library.h"
#include "rom.h"

// ---------------------------------------------------------------------------
// The SPI bus to the save chip

typedef struct {
    void *ctx;
    // Hold chip select, starting a transaction or carrying one on. slow
    // picks the 1 MHz clock instead of 4 MHz.
    void (*select)(void *ctx, bool slow);
    // Send one byte and return the byte clocked back.
    uint8_t (*transfer)(void *ctx, uint8_t out);
    // Release chip select, ending the transaction.
    void (*deselect)(void *ctx);
    void (*wait_us)(void *ctx, unsigned us);
    // Optional: false once the card has been pulled out. Long reads and
    // writes check it so they stop at once instead of timing out on a card
    // that is gone. NULL means "can't tell".
    bool (*present)(void *ctx);
} CardBus;

typedef enum {
    CHIP_NONE,        // Nothing answered
    CHIP_EEPROM_TINY, // 512 B EEPROM; address bit 8 goes in the command
    CHIP_EEPROM,      // 8 KB to 128 KB EEPROM
    CHIP_FLASH,       // 128 KB to 8 MB FLASH; erased in 64 KB sectors before writing
} ChipType;

typedef struct {
    ChipType type;
    bool infrared;    // An infrared chip sits in front of the save (Pokemon
                      // HG/SS and B/W, Personal Trainer: Walking)
    int addr_bytes;   // 1, 2 or 3
    int page_size;    // Largest write in one command
    long size;        // Bytes
    bool size_guessed;// Blank 2-byte EEPROM: 8 KB or 64 KB can't be told by
                      // reading, so size is 64 KB until a restore tests it
    bool writable;    // False for the 8 MB chip and unknown FLASH chips
    uint32_t jedec;   // FLASH ID; 0xFFFFFF for EEPROM
    uint8_t status;   // Status register when detected
} CardChip;

#define CARD_FLASH_SECTOR 0x10000

// Identify the save chip without writing to it. infrared is a hint (game
// codes starting with 'I'); the other mode is tried if the hint finds
// nothing. Returns false (type CHIP_NONE) if no chip answers.
bool card_chip_detect(CardBus *bus, bool infrared, CardChip *chip);

void card_chip_read(CardBus *bus, const CardChip *chip, uint32_t addr, uint8_t *buf, uint32_t len);

// Write len bytes at addr. For FLASH, addr and len must be whole 64 KB
// sectors; each sector is erased first. Pages that are already all 0xFF
// after an erase are skipped. Returns false if the chip stays busy.
bool card_chip_write(CardBus *bus, const CardChip *chip, uint32_t addr, const uint8_t *buf,
                     uint32_t len);

// Settle a guessed EEPROM size by writing one byte and seeing whether it
// shows up 8 KB later, then putting the byte back.
bool card_chip_test_size(CardBus *bus, CardChip *chip);

// "FLASH 512 KB", "EEPROM 8 KB", with " + IR" for infrared cards.
void card_chip_describe(const CardChip *chip, char *out, size_t size);

// ---------------------------------------------------------------------------
// The card in the slot

typedef enum {
    CARD_UNAVAILABLE, // Slot 1 can't be used here (DS mode, or run from a flashcart)
    CARD_EMPTY,       // No card in the slot
    CARD_UNREADABLE,  // The header did not read back correctly
    CARD_NO_CHIP,     // A card with no save chip that DSavE can find
    CARD_NAND,        // Saves to NAND inside the card (WarioWare D.I.Y. and a few
                      // others), which DSavE does not support yet
    CARD_READY,       // A card with a save chip
} CardStatus;

#define CARD_HEADER_SIZE 0x200

typedef struct {
    void *ctx;
    // Power slot 1 up, power-cycling it first so the card answers in its
    // start-up mode, and read the card's header and ROM chip ID. Returns
    // CARD_READY when the header was read; the CRC is checked by the caller.
    CardStatus (*open)(void *ctx, uint8_t *header, uint32_t *rom_id);
    CardBus bus;
} CardSlot;

typedef struct {
    CardStatus status;
    RomInfo info;     // From the header (game cards give no banner without
                      // a full card init, so header_title only)
    uint8_t header[CARD_HEADER_SIZE];
    uint32_t rom_id;
    CardChip chip;
} CardInfo;

// Look at what is in the slot. slot may be NULL (CARD_UNAVAILABLE).
void card_probe(CardSlot *slot, CardInfo *out);

// Put the card at the top of the library as a Game with is_card set, or
// remove it if card is NULL or CARD_UNAVAILABLE. The card's title is the
// title of a ROM on the SD card with the same game code, else the title of
// an existing backup folder for the code, else the header's title, so its
// backups share a folder with the ROM's.
void card_add_to_library(Library *lib, const CardInfo *card);

// Progress of a long card operation, for a progress bar.
typedef struct {
    void *ctx;
    void (*update)(void *ctx, long done, long total);
} CardProgress;

// Back up the card's save. The slot is re-opened first and must still hold
// the same card (else DSAVE_ERR_CARD_CHANGED). The chip is read twice and
// both reads must agree, which catches a loose card.
DsaveResult card_backup(const Library *lib, CardSlot *slot, const Game *card, const char *name,
                        bool allow_blank, Backup *out, const CardProgress *progress);

// Restore a backup onto the card:
//   1. validate the backup (size, CRC-32, game code) and check the card,
//   2. back the card's save up automatically ("auto_<timestamp>"),
//   3. write only the parts that differ (FLASH sectors are erased first),
//   4. read it all back and compare; on a mismatch, put the old save back.
// A backup larger than the chip is accepted if the extra is 0xFF padding
// (flashcarts pad saves to 512 KB); a smaller one is padded with 0xFF.
DsaveResult card_restore(const Library *lib, CardSlot *slot, Game *card, const Backup *b,
                         Backup *auto_backup, const CardProgress *progress);

#endif
