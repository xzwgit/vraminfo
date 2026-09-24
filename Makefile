CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS ?= -ldl
PREFIX  ?= /usr/local

all: vraminfo

vraminfo: vraminfo.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: vraminfo
	install -m 0755 vraminfo $(DESTDIR)$(PREFIX)/bin/vraminfo

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/vraminfo

clean:
	rm -f vraminfo

.PHONY: all install uninstall clean
