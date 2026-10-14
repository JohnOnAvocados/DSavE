#include "fake_card.h"

#include <string.h>

#define CMD_PP   0x02
#define CMD_READ 0x03
#define CMD_WRDI 0x04
#define CMD_RDSR 0x05
#define CMD_WREN 0x06
#define CMD_SE   0xD8
#define CMD_RDID 0x9F

static uint16_t crc16(const uint8_t *p, int n)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < n; i++)
    {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    return crc;
}

void fake_card_init(FakeCard *c, const char *code, const char *title, FakeChipType type,
                    long size, uint32_t jedec, bool infrared)
{
    memset(c, 0, sizeof(*c));
    c->slot_status = CARD_READY;
    memcpy(c->header, title, strlen(title) < 12 ? strlen(title) : 12);
    memcpy(c->header + 0x0C, code, 4);
    uint16_t crc = crc16(c->header, 0x15E);
    c->header[0x15E] = (uint8_t)crc;
    c->header[0x15F] = (uint8_t)(crc >> 8);
    c->rom_id = 0x00001FC2;
    c->type = type;
    c->size = size;
    c->infrared = infrared;
    c->jedec = type == FAKE_FLASH ? jedec : 0xFFFFFF;
    c->addr_bytes = type == FAKE_EEPROM_TINY ? 1 : (type == FAKE_EEPROM && size <= 65536) ? 2 : 3;
    c->idle_status = type == FAKE_EEPROM_TINY ? 0xF0 : (type == FAKE_EEPROM && size > 65536) ? 0x02 : 0x00;
    memset(c->mem, 0xFF, sizeof(c->mem));
}

static int page_size(const FakeCard *c)
{
    if (c->type == FAKE_EEPROM_TINY)
        return 16;
    if (c->type == FAKE_EEPROM && c->addr_bytes == 2)
        return 32;
    return 256;
}

static void fake_select(void *ctx, bool slow)
{
    FakeCard *c = ctx;
    if (!c->selected)
    {
        c->selected = true;
        c->ir_open = false;
        c->ir_seen_byte = false;
        c->pos = 0;
        c->addr = 0;
        c->page_len = 0;
    }
    c->slow = slow;
}

static void fake_deselect(void *ctx)
{
    FakeCard *c = ctx;
    if (!c->selected)
        return;
    c->selected = false;
    if (c->removed)
        return;
    bool started = c->pos > 0 && (!c->infrared || c->ir_open);
    if (!started || c->wip > 0)
        return;
    if (c->cmd == CMD_WREN)
        c->wel = true;
    else if (c->cmd == CMD_WRDI)
        c->wel = false;
    else if (c->cmd == CMD_PP && c->wel && c->pos > c->addr_bytes && !c->protect)
    {
        int ps = page_size(c);
        uint32_t base = c->page_addr & ~(uint32_t)(ps - 1);
        for (int i = 0; i < c->page_len; i++)
        {
            uint32_t a = (base | ((c->page_addr + (uint32_t)i) & (uint32_t)(ps - 1))) % (uint32_t)c->size;
            if (c->type == FAKE_FLASH)
                c->mem[a] &= c->page[i]; // FLASH programming only clears bits
            else
                c->mem[a] = c->page[i];
        }
        c->page_writes++;
        c->wel = false;
        c->wip = 2;
    }
    else if (c->cmd == CMD_SE && c->type == FAKE_FLASH && c->wel && c->pos == 4 && !c->protect)
    {
        uint32_t base = (c->addr % (uint32_t)c->size) & ~0xFFFFu;
        memset(c->mem + base, 0xFF, 0x10000);
        c->erases++;
        c->wel = false;
        c->wip = 3;
    }
}

static void maybe_remove(FakeCard *c)
{
    if ((c->remove_after_erases > 0 && c->erases >= c->remove_after_erases) ||
        (c->remove_after_reads > 0 && c->reads >= c->remove_after_reads))
        c->removed = true;
}

static uint8_t fake_transfer(void *ctx, uint8_t out)
{
    FakeCard *c = ctx;
    maybe_remove(c);
    if (!c->selected || c->type == FAKE_NONE || c->removed)
        return 0xFF;
    if (c->infrared && !c->ir_open)
    {
        // The IR chip opens the way to the save chip after a 0x00 at 1 MHz
        // as the first byte; anything else is swallowed.
        if (!c->ir_seen_byte && out == 0x00 && c->slow)
            c->ir_open = true;
        c->ir_seen_byte = true;
        return 0xFF;
    }

    int pos = c->pos++;
    if (pos == 0)
    {
        c->cmd = out;
        if (c->type == FAKE_EEPROM_TINY && ((out & 0xF7) == CMD_READ || (out & 0xF7) == CMD_PP))
        {
            c->cmd = out & 0xF7;
            c->addr = (out >> 3) & 1;
        }
        if (c->wip > 0 && c->cmd != CMD_RDSR)
            c->cmd = 0; // Busy: everything but a status read is ignored
        if (c->cmd == CMD_READ)
            c->reads++;
        return 0xFF;
    }

    switch (c->cmd)
    {
        case CMD_RDSR:
        {
            uint8_t sr = (uint8_t)(c->idle_status | (c->wel ? 0x02 : 0) | (c->wip > 0 ? 0x01 : 0));
            if (c->wip > 0)
                c->wip--;
            return sr;
        }
        case CMD_RDID:
            if (pos > 3)
                return 0xFF;
            return (uint8_t)(c->jedec >> ((3 - pos) * 8));
        case CMD_READ:
        case CMD_PP:
        case CMD_SE:
            if (pos <= c->addr_bytes)
            {
                c->addr = (c->addr << 8) | out;
                if (pos == c->addr_bytes)
                    c->page_addr = c->addr;
                return 0xFF;
            }
            if (c->cmd == CMD_READ)
            {
                uint8_t v = c->mem[c->addr++ % (uint32_t)c->size];
                if (c->reads == c->flaky_read && pos == c->addr_bytes + 1)
                    v ^= 1;
                return v;
            }
            if (c->cmd == CMD_PP && c->page_len < (int)sizeof(c->page))
                c->page[c->page_len++] = out;
            return 0xFF;
    }
    return 0xFF;
}

static void fake_wait(void *ctx, unsigned us)
{
    FakeCard *c = ctx;
    c->waited_us += us;
}

static bool fake_present(void *ctx)
{
    FakeCard *c = ctx;
    maybe_remove(c);
    return !c->removed;
}

static CardStatus fake_open(void *ctx, uint8_t *header, uint32_t *rom_id)
{
    FakeCard *c = ctx;
    c->opens++;
    c->selected = false;
    c->wel = false;
    c->wip = 0;
    if (c->removed)
        return CARD_EMPTY;
    if (c->slot_status != CARD_READY)
        return c->slot_status;
    memcpy(header, c->header, CARD_HEADER_SIZE);
    *rom_id = c->rom_id;
    return CARD_READY;
}

void fake_card_slot(FakeCard *c, CardSlot *slot)
{
    slot->ctx = c;
    slot->open = fake_open;
    slot->bus.ctx = c;
    slot->bus.select = fake_select;
    slot->bus.transfer = fake_transfer;
    slot->bus.deselect = fake_deselect;
    slot->bus.wait_us = fake_wait;
    slot->bus.present = fake_present;
}
