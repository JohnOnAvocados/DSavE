#include "card_slot.h"

#include <nds.h>
#include <string.h>

static void wait_frames(int n)
{
    for (int i = 0; i < n; i++)
        swiWaitForVBlank();
}

static void bus_select(void *ctx, bool slow)
{
    (void)ctx;
    REG_AUXSPICNT = CARD_ENABLE | CARD_SPI_ENABLE | CARD_SPI_HOLD |
                    (slow ? CARD_SPI_BAUD_1MHz : CARD_SPI_BAUD_4MHz);
    eepromWaitBusy();
}

static uint8_t bus_transfer(void *ctx, uint8_t out)
{
    (void)ctx;
    REG_AUXSPIDATA = out;
    eepromWaitBusy();
    return REG_AUXSPIDATA;
}

static void bus_deselect(void *ctx)
{
    (void)ctx;
    REG_AUXSPICNT = CARD_SPI_HOLD;
    eepromWaitBusy();
}

static void bus_wait_us(void *ctx, unsigned us)
{
    (void)ctx;
    swiDelay(us * 8); // About 8 loops per microsecond on the ARM9, as GodMode9i uses
}

static CardStatus slot_open(void *ctx, uint8_t *header, uint32_t *rom_id)
{
    (void)ctx;
    if (!isDSiMode())
        return CARD_UNAVAILABLE;
    if (REG_SCFG_MC & SCFG_MC_EJECTED)
        return CARD_EMPTY;

    sysSetCardOwner(BUS_OWNER_ARM9);

    // Power-cycle the slot so the card is back in its start-up mode, where
    // it answers header reads; the menu that launched DSavE may have left
    // it in its encrypted mode.
    disableSlot1();
    wait_frames(25);
    enableSlot1();
    wait_frames(15);
    if (REG_SCFG_MC & SCFG_MC_EJECTED)
        return CARD_EMPTY;

    static uint32_t buf[CARD_HEADER_SIZE / 4];
    memset(buf, 0, sizeof(buf));
    cardReadHeader((u8 *)buf); // Resets the card, then reads 0x200 bytes
    memcpy(header, buf, CARD_HEADER_SIZE);
    *rom_id = cardReadID(CARD_CLK_SLOW);
    return CARD_READY;
}

static CardSlot slot = {
    .ctx = NULL,
    .open = slot_open,
    .bus = { NULL, bus_select, bus_transfer, bus_deselect, bus_wait_us },
};

CardSlot *card_slot_get(const char *root)
{
    if (!isDSiMode() || strncmp(root, "sd:", 3) != 0)
        return NULL;
    return &slot;
}
