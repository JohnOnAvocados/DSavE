// A pretend game card for the tests: a header, a ROM chip ID and a save
// chip (EEPROM or FLASH, optionally behind an infrared chip) that answers
// the SPI commands the way the real chips do, including address wrapping,
// write-enable latches and FLASH only clearing bits until erased.

#ifndef DSAVE_FAKE_CARD_H
#define DSAVE_FAKE_CARD_H

#include <stdbool.h>
#include <stdint.h>

#include "../source/core/cardsave.h"

typedef enum {
    FAKE_NONE,
    FAKE_EEPROM_TINY, // 512 B
    FAKE_EEPROM,      // 2-byte address (8 KB or 64 KB), or 3-byte (128 KB)
    FAKE_FLASH,
} FakeChipType;

typedef struct {
    // Set up by the test
    CardStatus slot_status; // What open() reports (CARD_READY: card present)
    uint8_t header[CARD_HEADER_SIZE];
    uint32_t rom_id;
    FakeChipType type;
    long size;
    int addr_bytes;
    uint32_t jedec;
    uint8_t idle_status;
    bool infrared;
    bool protect;           // Writes and erases are silently ignored
    int flaky_read;         // This read command (1 = first) gets one wrong byte
    bool removed;           // Pulled out: every byte reads 0xFF and nothing is written
    int remove_after_erases;// > 0: pull the card out once this many sectors were erased
    int remove_after_reads; // > 0: pull the card out once this many read commands started
    long waited_us;         // Total of the bus's waits
    uint8_t mem[1024 * 1024];

    // Counters
    int opens, page_writes, erases;

    // Transaction state
    bool selected, slow, ir_open, ir_seen_byte, wel;
    int wip;
    int pos;                // Bytes in this transaction so far
    uint8_t cmd;
    uint32_t addr;
    uint8_t page[256];
    int page_len;
    uint32_t page_addr;
    int reads;
} FakeCard;

// A game card with a valid header for the game code and title.
void fake_card_init(FakeCard *c, const char *code, const char *title, FakeChipType type,
                    long size, uint32_t jedec, bool infrared);

// The slot that talks to it.
void fake_card_slot(FakeCard *c, CardSlot *slot);

#endif
