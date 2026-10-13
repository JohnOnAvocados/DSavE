# DSavE: build with BlocksDS (https://blocksds.skylyrac.net)
#   make            -> DSavE.nds
#   make -C tests   -> host-side engine tests

NAME          := DSavE
GAME_TITLE    := DSavE
GAME_SUBTITLE := DS save manager
GAME_AUTHOR   := JohnOnAvocados

SOURCEDIRS    := source
INCLUDEDIRS   := source
# Truncating long names into fixed buffers is deliberate.
DEFINES       := -Wno-format-truncation

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile
