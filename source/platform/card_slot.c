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

static bool card_out(void)
{
    return (REG_SCFG_MC & SCFG_MC_EJECTED) != 0;
}

static bool bus_present(void *ctx)
{
    (void)ctx;
    return !card_out();
}

static void bus_wait_us(void *ctx, unsigned us)
{
    (void)ctx;
    swiDelay(us * 8); // About 8 loops per microsecond on the ARM9, as GodMode9i uses
}

// The ROM bus reads below follow libnds's cardReadHeader() and cardReadID(),
// whose wait loops have no way out. These give up if the card comes out or
// the transfer never finishes, so pulling the card can't hang the console.
// (Unconfirmed on hardware whether the libnds loops really hang; these
// limits are far above a normal transfer, which takes a few milliseconds.)
#define ROM_SPIN_LIMIT 0x400000

// Run one ROM transfer, keeping up to `words` words of its data. False on a
// timeout or a pulled card.
static bool rom_transfer(const u8 *command, u32 flags, u32 *dst, u32 words)
{
    REG_AUXSPICNTH = CARD_SPICNTH_ENABLE | CARD_SPICNTH_IRQ;
    for (int i = 0; i < 8; i++)
        REG_CARD_COMMAND[7 - i] = command[i];
    REG_ROMCTRL = flags;
    for (u32 spins = 0; REG_ROMCTRL & CARD_BUSY; spins++)
    {
        if (REG_ROMCTRL & CARD_DATA_READY)
        {
            u32 data = REG_CARD_DATA_RD;
            if (dst && words)
            {
                *dst++ = data;
                words--;
            }
        }
        if (spins >= ROM_SPIN_LIMIT || ((spins & 0xFF) == 0 && card_out()))
        {
            REG_ROMCTRL = 0;
            return false;
        }
    }
    return true;
}

static bool rom_idle(void)
{
    for (u32 spins = 0; REG_ROMCTRL & CARD_BUSY; spins++)
    {
        if (spins >= ROM_SPIN_LIMIT || card_out())
            return false;
    }
    return true;
}

// The header (0x200 bytes) and the ROM chip ID, read in the card's start-up
// mode.
static bool read_header_and_id(u32 *header, u32 *rom_id)
{
    REG_ROMCTRL = 0;
    REG_AUXSPICNTH = 0;
    swiDelay(167550);
    REG_AUXSPICNTH = CARD_SPICNTH_ENABLE | CARD_SPICNTH_IRQ;
    REG_ROMCTRL = CARD_nRESET | CARD_SEC_SEED;
    if (!rom_idle())
        return false;

    static const u8 dummy[8] = { 0, 0, 0, 0, 0, 0, 0, CARD_CMD_DUMMY };
    if (!rom_transfer(dummy, CARD_ACTIVATE | CARD_nRESET | CARD_CLK_SLOW | CARD_BLK_SIZE(5) | CARD_DELAY2(0x18),
                      NULL, 0))
        return false;

    static const u8 read_header[8] = { 0, 0, 0, 0, 0, 0, 0, CARD_CMD_HEADER_READ };
    if (!rom_transfer(read_header,
                      CARD_ACTIVATE | CARD_nRESET | CARD_CLK_SLOW | CARD_BLK_SIZE(1) | CARD_DELAY1(0x1FFF) |
                          CARD_DELAY2(0x3F),
                      header, CARD_HEADER_SIZE / 4))
        return false;

    static const u8 chip_id[8] = { 0, 0, 0, 0, 0, 0, 0, CARD_CMD_HEADER_CHIPID };
    u32 id = 0;
    if (!rom_transfer(chip_id, CARD_ACTIVATE | CARD_nRESET | CARD_CLK_SLOW | CARD_BLK_SIZE(7), &id, 1))
        return false;
    *rom_id = id;
    return true;
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
    if (!read_header_and_id(buf, rom_id))
        return card_out() ? CARD_EMPTY : CARD_UNREADABLE;
    memcpy(header, buf, CARD_HEADER_SIZE);
    return CARD_READY;
}

static CardSlot slot = {
    .ctx = NULL,
    .open = slot_open,
    .bus = { NULL, bus_select, bus_transfer, bus_deselect, bus_wait_us, bus_present },
};

CardSlot *card_slot_get(const char *root)
{
    if (!isDSiMode() || strncmp(root, "sd:", 3) != 0)
        return NULL;
    return &slot;
}
