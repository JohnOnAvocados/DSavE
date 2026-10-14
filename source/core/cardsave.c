#include "cardsave.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "savefile.h"

#define CMD_PP    0x02 // Page program
#define CMD_READ  0x03
#define CMD_RDSR  0x05
#define CMD_WREN  0x06
#define CMD_SE    0xD8 // Sector erase (FLASH, 64 KB)
#define CMD_RDID  0x9F

#define SR_WIP 0x01 // Write in progress

#define KB 1024L

// Work happens in 64 KB pieces: one FLASH sector, and all of any EEPROM.
#define CHUNK CARD_FLASH_SECTOR
static uint8_t chunk_a[CHUNK], chunk_b[CHUNK];

// ---------------------------------------------------------------------------
// Transactions

// Start a command. Infrared cards pass the save chip's commands on only
// after a 0x00 byte sent at 1 MHz, with pauses either side; the timings are
// GodMode9i's, which work on real HG/SS and B/W cards.
static void begin(CardBus *bus, bool ir)
{
    if (ir)
    {
        bus->select(bus->ctx, false);
        bus->wait_us(bus->ctx, 150);
        bus->select(bus->ctx, true);
        bus->transfer(bus->ctx, 0x00);
        bus->wait_us(bus->ctx, 150);
        bus->wait_us(bus->ctx, 16700);
    }
    bus->select(bus->ctx, false);
}

static void end(CardBus *bus)
{
    bus->deselect(bus->ctx);
}

static uint8_t read_status(CardBus *bus, bool ir)
{
    begin(bus, ir);
    bus->transfer(bus->ctx, CMD_RDSR);
    uint8_t sr = bus->transfer(bus->ctx, 0);
    end(bus);
    return sr;
}

static uint32_t read_jedec(CardBus *bus, bool ir)
{
    begin(bus, ir);
    bus->transfer(bus->ctx, CMD_RDID);
    uint32_t id = 0;
    for (int i = 0; i < 3; i++)
        id = (id << 8) | bus->transfer(bus->ctx, 0);
    end(bus);
    return id;
}

// Command byte plus address. The 512 B EEPROM takes address bit 8 in bit 3
// of the command and one address byte.
static void send_command(CardBus *bus, const CardChip *chip, uint8_t cmd, uint32_t addr)
{
    if (chip->type == CHIP_EEPROM_TINY)
        cmd |= (uint8_t)(((addr >> 8) & 1) << 3);
    bus->transfer(bus->ctx, cmd);
    for (int i = chip->addr_bytes - 1; i >= 0; i--)
        bus->transfer(bus->ctx, (uint8_t)(addr >> (i * 8)));
}

// True once the card is known to be out of the slot.
static bool card_gone(CardBus *bus)
{
    return bus->present && !bus->present(bus->ctx);
}

static bool wait_ready(CardBus *bus, const CardChip *chip, unsigned timeout_ms)
{
    begin(bus, chip->infrared);
    bus->transfer(bus->ctx, CMD_RDSR);
    bool ready = false;
    for (unsigned polls = 0; polls <= timeout_ms * 20 && !ready && !card_gone(bus); polls++)
    {
        ready = !(bus->transfer(bus->ctx, 0) & SR_WIP);
        if (!ready)
            bus->wait_us(bus->ctx, 50);
    }
    end(bus);
    return ready;
}

static void write_enable(CardBus *bus, const CardChip *chip)
{
    begin(bus, chip->infrared);
    bus->transfer(bus->ctx, CMD_WREN);
    end(bus);
}

void card_chip_read(CardBus *bus, const CardChip *chip, uint32_t addr, uint8_t *buf, uint32_t len)
{
    while (len)
    {
        // The 512 B EEPROM's read wraps at 256 bytes on some chips.
        uint32_t n = len;
        if (chip->type == CHIP_EEPROM_TINY && (addr & 0xFF) + n > 0x100)
            n = 0x100 - (addr & 0xFF);
        begin(bus, chip->infrared);
        send_command(bus, chip, CMD_READ, addr);
        for (uint32_t i = 0; i < n; i++)
            buf[i] = bus->transfer(bus->ctx, 0);
        end(bus);
        addr += n;
        buf += n;
        len -= n;
    }
}

static bool all_ff(const uint8_t *p, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
    {
        if (p[i] != 0xFF)
            return false;
    }
    return true;
}

static bool erase_sector(CardBus *bus, const CardChip *chip, uint32_t addr)
{
    write_enable(bus, chip);
    begin(bus, chip->infrared);
    bus->transfer(bus->ctx, CMD_SE);
    for (int i = 2; i >= 0; i--)
        bus->transfer(bus->ctx, (uint8_t)(addr >> (i * 8)));
    end(bus);
    return wait_ready(bus, chip, 5000);
}

bool card_chip_write(CardBus *bus, const CardChip *chip, uint32_t addr, const uint8_t *buf,
                     uint32_t len)
{
    bool flash = chip->type == CHIP_FLASH;
    if (flash && ((addr % CARD_FLASH_SECTOR) || (len % CARD_FLASH_SECTOR)))
        return false;
    uint32_t end_addr = addr + len;
    while (addr < end_addr)
    {
        if (flash && addr % CARD_FLASH_SECTOR == 0 && !erase_sector(bus, chip, addr))
            return false;
        uint32_t page = (uint32_t)chip->page_size;
        uint32_t n = page - (addr % page);
        if (n > end_addr - addr)
            n = end_addr - addr;
        if (!(flash && all_ff(buf, n)))
        {
            write_enable(bus, chip);
            begin(bus, chip->infrared);
            send_command(bus, chip, CMD_PP, addr);
            for (uint32_t i = 0; i < n; i++)
                bus->transfer(bus->ctx, buf[i]);
            end(bus);
            if (!wait_ready(bus, chip, 100))
                return false;
        }
        addr += n;
        buf += n;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Detection

static long flash_size(uint32_t id)
{
    switch (id)
    {
        case 0xC22211:                   return 128 * KB; // Macronix MX25L1021E
        case 0x204012: case 0x621600:    return 256 * KB;
        case 0x204013: case 0x208013:
        case 0x621100:                   return 512 * KB;
        case 0x204014:                   return 1024 * KB;
        case 0x202017: case 0x204017:
        case 0xC22017:                   return 8192 * KB; // Band Brothers DX
    }
    return 0;
}

// True if the chip reads the same at addr and addr + period for every
// address from period up to limit, i.e. its address space wraps at period.
static bool wraps_at(CardBus *bus, const CardChip *chip, long period, long limit)
{
    uint32_t step = (uint32_t)(period < CHUNK ? period : CHUNK);
    for (long off = period; off < limit; off += step)
    {
        card_chip_read(bus, chip, (uint32_t)(off - period), chunk_a, step);
        card_chip_read(bus, chip, (uint32_t)off, chunk_b, step);
        if (memcmp(chunk_a, chunk_b, step) != 0)
            return false;
    }
    return true;
}

static bool uniform(CardBus *bus, const CardChip *chip, long len)
{
    uint32_t n = (uint32_t)(len < CHUNK ? len : CHUNK);
    card_chip_read(bus, chip, 0, chunk_a, n);
    for (uint32_t i = 1; i < n; i++)
    {
        if (chunk_a[i] != chunk_a[0])
            return false;
    }
    return true;
}

// The smallest of the candidate sizes at which the chip's addresses wrap.
// A chip that reads the same everywhere (blank) can't be told apart; the
// largest candidate is returned and *guessed set.
static long size_by_wrapping(CardBus *bus, const CardChip *chip, const long *sizes, int count,
                             bool *guessed)
{
    long limit = sizes[count - 1];
    *guessed = false;
    for (int i = 0; i + 1 < count; i++)
    {
        if (wraps_at(bus, chip, sizes[i], limit))
        {
            if (uniform(bus, chip, sizes[i]))
            {
                *guessed = true;
                return limit;
            }
            return sizes[i];
        }
    }
    return limit;
}

static bool detect(CardBus *bus, bool ir, CardChip *chip)
{
    memset(chip, 0, sizeof(*chip));
    chip->infrared = ir;
    uint8_t sr = read_status(bus, ir);
    uint32_t id = read_jedec(bus, ir);
    chip->status = sr;
    chip->jedec = id;

    if ((sr == 0xFF && id == 0xFFFFFF) || (sr == 0 && id == 0))
        return false;

    chip->writable = true;
    if (id == 0xFFFFFF && sr == 0xF0)
    {
        chip->type = CHIP_EEPROM_TINY;
        chip->addr_bytes = 1;
        chip->page_size = 16;
        chip->size = 512;
    }
    else if (id == 0xFFFFFF && sr == 0x00)
    {
        // 8 KB or 64 KB, with 16-bit addresses: the 8 KB chip repeats.
        static const long sizes[] = { 8 * KB, 64 * KB };
        chip->type = CHIP_EEPROM;
        chip->addr_bytes = 2;
        chip->page_size = 32;
        chip->size = size_by_wrapping(bus, chip, sizes, 2, &chip->size_guessed);
    }
    else if (id == 0xFFFFFF && sr == 0x02)
    {
        // 128 KB EEPROM with 24-bit addresses (Pokemon Mystery Dungeon:
        // Explorers of Sky), which reports this status at rest.
        chip->type = CHIP_EEPROM;
        chip->addr_bytes = 3;
        chip->page_size = 256;
        chip->size = 128 * KB;
    }
    else if (id != 0xFFFFFF)
    {
        chip->type = CHIP_FLASH;
        chip->addr_bytes = 3;
        chip->page_size = 256;
        chip->size = flash_size(id);
        if (chip->size == 8192 * KB)
            chip->writable = false; // Needs an unlock DSavE doesn't do yet
        if (!chip->size)
        {
            // Unknown chip: readable, size from wrapping, never written.
            static const long sizes[] = { 256 * KB, 512 * KB, 1024 * KB };
            chip->writable = false;
            chip->size = size_by_wrapping(bus, chip, sizes, 3, &chip->size_guessed);
        }
    }
    else
        return false;
    return true;
}

bool card_chip_detect(CardBus *bus, bool infrared, CardChip *chip)
{
    if (detect(bus, infrared, chip) || detect(bus, !infrared, chip))
        return true;
    memset(chip, 0, sizeof(*chip));
    return false;
}

bool card_chip_test_size(CardBus *bus, CardChip *chip)
{
    if (!chip->size_guessed)
        return true;
    if (chip->type != CHIP_EEPROM || chip->addr_bytes != 2)
        return false;
    uint8_t old, mark, back, mirror;
    card_chip_read(bus, chip, 0, &old, 1);
    mark = (uint8_t)~old;
    if (!card_chip_write(bus, chip, 0, &mark, 1))
        return false;
    card_chip_read(bus, chip, 0, &back, 1);
    card_chip_read(bus, chip, 8 * KB, &mirror, 1);
    bool ok = back == mark && card_chip_write(bus, chip, 0, &old, 1);
    if (!ok)
        return false;
    chip->size = mirror == mark ? 8 * KB : 64 * KB;
    chip->size_guessed = false;
    return true;
}

void card_chip_describe(const CardChip *chip, char *out, size_t size)
{
    const char *kind = chip->type == CHIP_FLASH ? "FLASH" : "EEPROM";
    if (chip->type == CHIP_NONE)
    {
        snprintf(out, size, "No save chip");
        return;
    }
    if (chip->size >= 1024 * KB)
        snprintf(out, size, "%s %ld MB%s", kind, chip->size / (1024 * KB), chip->infrared ? " + IR" : "");
    else if (chip->size >= KB)
        snprintf(out, size, "%s %ld KB%s", kind, chip->size / KB, chip->infrared ? " + IR" : "");
    else
        snprintf(out, size, "%s %ld B%s", kind, chip->size, chip->infrared ? " + IR" : "");
}

// ---------------------------------------------------------------------------
// The card in the slot

// Game codes of cards that keep their save in NAND (GodMode9i's list).
static bool nand_game(const char *code)
{
    static const char *const nand[] = { "UXB", "UOR", "USK", "UGD" };
    for (size_t i = 0; i < sizeof(nand) / sizeof(nand[0]); i++)
    {
        if (strncmp(code, nand[i], 3) == 0)
            return true;
    }
    return false;
}

void card_probe(CardSlot *slot, CardInfo *out)
{
    memset(out, 0, sizeof(*out));
    if (!slot)
    {
        out->status = CARD_UNAVAILABLE;
        return;
    }
    out->status = slot->open(slot->ctx, out->header, &out->rom_id);
    if (out->status != CARD_READY)
        return;
    if (!rom_header_crc_ok(out->header))
    {
        out->status = CARD_UNREADABLE;
        return;
    }
    rom_parse(&out->info, out->header, NULL);
    // Bit 27 of the ROM chip ID marks a card with NAND (GBATEK).
    if ((out->rom_id & (1u << 27)) || nand_game(out->info.game_code))
    {
        out->status = CARD_NAND;
        return;
    }
    if (!card_chip_detect(&slot->bus, out->info.game_code[0] == 'I', &out->chip))
        out->status = CARD_NO_CHIP;
}

// Title of an existing backup folder "<CODE> <title>", if there is one.
static bool folder_title(const Library *lib, const char *code, char *out, size_t size)
{
    char saves[DSAVE_PATH_MAX];
    if (!path_join(saves, sizeof(saves), lib->root, "_DSavE/saves"))
        return false;
    DIR *d = opendir(saves);
    if (!d)
        return false;
    bool found = false;
    struct dirent *e;
    size_t len = strlen(code);
    while (!found && (e = readdir(d)) != NULL)
    {
        if (strncmp(e->d_name, code, len) == 0 && e->d_name[len] == ' ' && e->d_name[len + 1])
        {
            snprintf(out, size, "%s", e->d_name + len + 1);
            found = true;
        }
    }
    closedir(d);
    return found;
}

void card_add_to_library(Library *lib, const CardInfo *card)
{
    if (lib->count && lib->games[0].is_card)
    {
        memmove(&lib->games[0], &lib->games[1], sizeof(Game) * (size_t)(lib->count - 1));
        lib->count--;
    }
    if (!card || card->status == CARD_UNAVAILABLE)
        return;

    if (lib->count == LIBRARY_MAX_GAMES)
    {
        lib->count--;
        lib->truncated = true;
    }
    memmove(&lib->games[1], &lib->games[0], sizeof(Game) * (size_t)lib->count);
    lib->count++;

    Game *g = &lib->games[0];
    memset(g, 0, sizeof(*g));
    g->is_card = true;
    g->info = card->info;
    g->chip_size = -1;
    g->save_size = -1;
    if (card->status == CARD_READY)
    {
        g->has_save = true;
        g->save_size = card->chip.size;
    }

    const char *code = card->info.game_code;
    if (!code[0])
    {
        snprintf(g->title, sizeof(g->title), "Game card");
        return;
    }
    for (int i = 1; i < lib->count; i++)
    {
        if (strcmp(lib->games[i].info.game_code, code) == 0)
        {
            snprintf(g->title, sizeof(g->title), "%s", lib->games[i].title);
            return;
        }
    }
    if (folder_title(lib, code, g->title, sizeof(g->title)))
        return;
    snprintf(g->title, sizeof(g->title), "%s",
             card->info.header_title[0] ? card->info.header_title : "Game card");
}

// ---------------------------------------------------------------------------
// Backup and restore

typedef struct {
    const CardProgress *p;
    long done, total;
} Progress;

static void advance(Progress *pr, long n)
{
    pr->done += n;
    if (pr->p && pr->p->update)
        pr->p->update(pr->p->ctx, pr->done, pr->total);
}

// Re-open the slot and check the card is still the one in the library.
static DsaveResult reopen(CardSlot *slot, const Game *card, CardInfo *now)
{
    card_probe(slot, now);
    if (now->status != CARD_READY || strcmp(now->info.game_code, card->info.game_code) != 0)
        return DSAVE_ERR_CARD_CHANGED;
    return DSAVE_OK;
}

static uint32_t chunk_len(const CardChip *chip, long off)
{
    long n = chip->size - off;
    return (uint32_t)(n < CHUNK ? n : CHUNK);
}

// CRC-32 of the whole chip. False if the card was pulled out meanwhile.
static bool chip_crc(CardBus *bus, const CardChip *chip, Progress *pr, uint32_t *crc_out)
{
    uint32_t crc = 0;
    for (long off = 0; off < chip->size; off += CHUNK)
    {
        uint32_t n = chunk_len(chip, off);
        card_chip_read(bus, chip, (uint32_t)off, chunk_a, n);
        if (card_gone(bus))
            return false;
        crc = crc32_update(crc, chunk_a, n);
        advance(pr, n);
    }
    *crc_out = crc;
    return true;
}

// Copy the chip to a file, then read it again: both reads must agree.
static DsaveResult dump_chip(CardBus *bus, const CardChip *chip, const char *path, Progress *pr)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return DSAVE_ERR_IO;
    uint32_t crc = 0, again;
    bool ok = true, gone = false;
    for (long off = 0; ok && off < chip->size; off += CHUNK)
    {
        uint32_t n = chunk_len(chip, off);
        card_chip_read(bus, chip, (uint32_t)off, chunk_a, n);
        // A pulled card reads as all 0xFF twice over, which would pass the
        // second read below and make a blank backup.
        gone = card_gone(bus);
        if (gone)
            break;
        crc = crc32_update(crc, chunk_a, n);
        ok = fwrite(chunk_a, 1, n, f) == n;
        advance(pr, n);
    }
    if (fclose(f) != 0 || !ok)
        return gone ? DSAVE_ERR_CARD_CHANGED : DSAVE_ERR_IO;
    if (gone || !chip_crc(bus, chip, pr, &again))
        return DSAVE_ERR_CARD_CHANGED;
    return again == crc ? DSAVE_OK : DSAVE_ERR_CARD;
}

static bool temp_path(const Library *lib, char *out, size_t size)
{
    char dir[DSAVE_PATH_MAX];
    return path_join(dir, sizeof(dir), lib->root, "_DSavE") && mkdir_p(dir) &&
           path_join(out, size, dir, ".card.tmp");
}

static DsaveResult backup_chip(const Library *lib, CardBus *bus, const CardChip *chip,
                               const Game *card, const char *name, bool allow_blank, Backup *out,
                               Progress *pr)
{
    char tmp[DSAVE_PATH_MAX], label[64], desc[32];
    if (!temp_path(lib, tmp, sizeof(tmp)))
        return DSAVE_ERR_PATH;
    DsaveResult r = dump_chip(bus, chip, tmp, pr);
    if (r == DSAVE_OK)
    {
        card_chip_describe(chip, desc, sizeof(desc));
        snprintf(label, sizeof(label), "game card (%s)", desc);
        r = backup_create_labeled(lib, card, tmp, label, name, allow_blank, out);
    }
    remove(tmp);
    return r;
}

DsaveResult card_backup(const Library *lib, CardSlot *slot, const Game *card, const char *name,
                        bool allow_blank, Backup *out, const CardProgress *progress)
{
    CardInfo now;
    DsaveResult r = reopen(slot, card, &now);
    if (r != DSAVE_OK)
        return r;
    Progress pr = { progress, 0, now.chip.size * 2 };
    return backup_chip(lib, &slot->bus, &now.chip, card, name, allow_blank, out, &pr);
}

// Write a save file's data (raw or .gz) to the whole chip, padding with
// 0xFF, skipping chunks that already match. data_len is the file's save
// length: a read that comes back short before it is an SD error, not the
// end of the data. crc_out is the CRC-32 of what the chip should now hold.
static DsaveResult write_chip(CardBus *bus, const CardChip *chip, const char *path, long data_len,
                              uint32_t *crc_out, Progress *pr)
{
    SaveReader in;
    if (!savefile_open(&in, path))
        return DSAVE_ERR_IO;
    uint32_t crc = 0;
    DsaveResult r = DSAVE_OK;
    for (long off = 0; r == DSAVE_OK && off < chip->size; off += CHUNK)
    {
        uint32_t n = chunk_len(chip, off);
        long left = data_len - off;
        size_t expect = left <= 0 ? 0 : left < (long)n ? (size_t)left : n;
        size_t got = savefile_read(&in, chunk_a, n);
        if (got < expect)
        {
            r = DSAVE_ERR_IO;
            break;
        }
        memset(chunk_a + got, 0xFF, n - got);
        crc = crc32_update(crc, chunk_a, n);
        card_chip_read(bus, chip, (uint32_t)off, chunk_b, n);
        if (memcmp(chunk_a, chunk_b, n) != 0 && !card_chip_write(bus, chip, (uint32_t)off, chunk_a, n))
            r = DSAVE_ERR_CARD;
        if (card_gone(bus))
            r = DSAVE_ERR_CARD_CHANGED;
        advance(pr, n);
    }
    savefile_close(&in);
    *crc_out = crc;
    return r;
}

DsaveResult card_restore(const Library *lib, CardSlot *slot, Game *card, const Backup *b,
                         Backup *auto_backup, const CardProgress *progress)
{
    if (auto_backup)
        memset(auto_backup, 0, sizeof(*auto_backup));
    DsaveResult r = backup_validate(b);
    if (r != DSAVE_OK)
        return r;
    if (b->game_code[0] && strcmp(b->game_code, card->info.game_code) != 0)
        return DSAVE_ERR_WRONG_GAME;

    CardInfo now;
    r = reopen(slot, card, &now);
    if (r != DSAVE_OK)
        return r;
    CardBus *bus = &slot->bus;
    CardChip *chip = &now.chip;
    if (!chip->writable)
        return DSAVE_ERR_UNSUPPORTED;
    // A guessed size is the largest the chip can be, so this check holds
    // before the size is settled; it is made again after.
    long size = savefile_size(b->file);
    if (size > chip->size && !savefile_tail_is_ff(b->file, chip->size))
        return DSAVE_ERR_BAD_SIZE;

    // Safety copy first (before the size test writes a byte); then write,
    // then read it all back.
    Progress pr = { progress, 0, chip->size * 4 };
    char auto_name[BACKUP_NAME_MAX + 1];
    backup_auto_name(auto_name, sizeof(auto_name));
    Backup safety;
    r = backup_chip(lib, bus, chip, card, auto_name, true, &safety, &pr);
    if (r != DSAVE_OK)
        return r;
    if (auto_backup)
        *auto_backup = safety;
    backup_prune_auto(lib, card, lib->keep_auto, safety.name);
    if (chip->size_guessed)
    {
        if (!card_chip_test_size(bus, chip))
            return DSAVE_ERR_CARD;
        if (size > chip->size && !savefile_tail_is_ff(b->file, chip->size))
            return DSAVE_ERR_BAD_SIZE;
        pr.total = pr.done + chip->size * 2;
    }

    uint32_t want, got;
    r = write_chip(bus, chip, b->file, size, &want, &pr);
    if (r == DSAVE_OK && chip_crc(bus, chip, &pr, &got) && got == want)
        return DSAVE_OK;

    // Put the old save back, and say so only if it reads back right. A card
    // that was pulled out can't be put back; the auto backup holds its save.
    Progress again = { NULL, 0, 0 };
    if (!card_gone(bus) && write_chip(bus, chip, safety.file, safety.size, &want, &again) == DSAVE_OK &&
        chip_crc(bus, chip, &again, &got) && got == want)
        return DSAVE_ERR_ROLLED_BACK;
    return DSAVE_ERR_ROLLBACK_FAILED;
}
