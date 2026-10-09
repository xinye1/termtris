CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -pedantic
PREFIX ?= $(HOME)/.local

# Windows (MSYS2/MinGW): .exe names and the audio library
ifeq ($(OS),Windows_NT)
EXE = .exe
LDLIBS += -lwinmm
endif

termtris$(EXE): termtris.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ termtris.c $(LDLIBS)

test: test.c termtris.c
	$(CC) $(CFLAGS) -Wno-unused-function $(LDFLAGS) -o termtris-test$(EXE) test.c $(LDLIBS)
	./termtris-test$(EXE)

install: termtris$(EXE)
	install -Dm755 termtris$(EXE) $(DESTDIR)$(PREFIX)/bin/termtris$(EXE)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/termtris$(EXE)

clean:
	rm -f termtris termtris-test termtris.exe termtris-test.exe

.PHONY: test install uninstall clean
