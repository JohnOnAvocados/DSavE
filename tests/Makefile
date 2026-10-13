# Host build of the save engine plus its tests: `make -C tests`
CC ?= cc
CFLAGS ?= -std=gnu99 -Wall -Wextra -Werror -Wno-format-truncation -O1 -g -fsanitize=address,undefined
SRC := $(wildcard ../source/core/*.c)
MINIZ := ../source/third_party/miniz/miniz_impl.c

.PHONY: test clean
test: test_core
	./test_core

test_core: test_core.c $(SRC) $(MINIZ) $(wildcard ../source/core/*.h)
	$(CC) $(CFLAGS) -o $@ test_core.c $(SRC) $(MINIZ)

clean:
	rm -f test_core
