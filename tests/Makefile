# Host build of the save engine plus its tests: `make -C tests`
CC ?= cc
CFLAGS ?= -std=gnu99 -Wall -Wextra -Werror -Wno-format-truncation -O1 -g -fsanitize=address,undefined
SRC := ../source/core/util.c ../source/core/rom.c ../source/core/library.c ../source/core/backup.c

.PHONY: test clean
test: test_core
	./test_core

test_core: test_core.c $(SRC) $(wildcard ../source/core/*.h)
	$(CC) $(CFLAGS) -o $@ test_core.c $(SRC)

clean:
	rm -f test_core
