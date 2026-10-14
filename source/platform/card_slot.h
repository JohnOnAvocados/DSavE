// Slot 1 on the console: power, the card's header, and the save chip's SPI
// bus (AUXSPI), for cardsave.c. Needs DSi mode with SCFG access, which DSi
// and 3DS homebrew launched from the SD card by TWiLight Menu++ or Unlaunch
// has.

#ifndef DSAVE_CARD_SLOT_H
#define DSAVE_CARD_SLOT_H

#include "core/cardsave.h"

// The console's slot 1, or NULL if it can't be used (not in DSi mode, or
// DSavE was not started from the SD card, so a flashcart is in the slot).
CardSlot *card_slot_get(const char *root);

#endif
