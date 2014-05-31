CC ?= cc
CFLAGS ?= -Wall -pedantic -std=gnu99
LDFLAGS ?=
CFLAGS += $(shell pkg-config --cflags fuse zlib)
LIBS = $(shell pkg-config --libs fuse zlib)
DESTDIR ?= /
SBIN_DIR ?= sbin
DOC_DIR ?= usr/share/doc/luufs
MAN_DIR ?= usr/share/man

luufs: luufs.c tree.c tree.h
	$(CC) $(CFLAGS) tree.c luufs.c $(LDFLAGS) $(LIBS) -o luufs

clean:
	rm -f luufs

install: luufs
	install -D -m 755 luufs $(DESTDIR)/$(SBIN_DIR)/luufs
	install -D -m 644 luufs.8 $(DESTDIR)/$(MAN_DIR)/man8/luufs.8
	install -D -m 644 README $(DESTDIR)/$(DOC_DIR)/README
	install -m 644 AUTHORS $(DESTDIR)/$(DOC_DIR)/AUTHORS
	install -m 644 COPYING $(DESTDIR)/$(DOC_DIR)/COPYING
