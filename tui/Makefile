CC      ?= cc
CFLAGS  ?= -O2 -Wall
LDFLAGS ?= -lpq -lncurses

# Termux: headers and libs live under $PREFIX
ifneq ($(PREFIX),)
    CFLAGS  += -I$(PREFIX)/include
    LDFLAGS := -L$(PREFIX)/lib $(LDFLAGS)
endif

admin-tui: admin-tui.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f admin-tui

.PHONY: clean
