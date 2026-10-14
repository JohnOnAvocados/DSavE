# Host build of the save engine and the UI, plus their tests: `make -C tests`
CC ?= cc
CFLAGS ?= -std=gnu99 -Wall -Wextra -Werror -Wno-format-truncation -O1 -g -fsanitize=address,undefined
SRC := $(wildcard ../source/core/*.c)
UI := $(wildcard ../source/ui/*.c)
MINIZ := ../source/third_party/miniz/miniz_impl.c
HEADERS := $(wildcard ../source/core/*.h ../source/ui/*.h)

.PHONY: test clean
test: test_core test_ui
	./test_core
	./test_ui screenshots

test_core: test_core.c fake_card.c fake_card.h $(SRC) $(MINIZ) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ test_core.c fake_card.c $(SRC) $(MINIZ)

test_ui: test_ui.c fake_card.c fake_card.h $(SRC) $(UI) $(MINIZ) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ test_ui.c fake_card.c $(SRC) $(UI) $(MINIZ)

clean:
	rm -rf test_core test_ui screenshots
