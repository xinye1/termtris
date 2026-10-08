CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -pedantic
PREFIX ?= $(HOME)/.local

termtris: termtris.c
	$(CC) $(CFLAGS) -o $@ termtris.c

test: test.c termtris.c
	$(CC) $(CFLAGS) -Wno-unused-function -o termtris-test test.c
	./termtris-test

install: termtris
	install -Dm755 termtris $(DESTDIR)$(PREFIX)/bin/termtris

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/termtris

clean:
	rm -f termtris termtris-test

.PHONY: test install uninstall clean
