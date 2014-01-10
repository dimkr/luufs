CC ?= cc
CFLAGS ?= -Wall -pedantic
LDFLAGS ?=
FUSE_CFLAGS = $(shell pkg-config --cflags fuse)
FUSE_LIBS = $(shell pkg-config --libs fuse)
DESTDIR ?= /
SBIN_DIR ?= sbin
DOC_DIR ?= usr/share/doc/luufs

luufs: luufs.c config.h
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) luufs.c $(LDFLAGS) $(FUSE_LIBS) -o luufs

clean:
	rm -f luufs

install: luufs
	install -D -m 755 luufs $(DESTDIR)/$(SBIN_DIR)/luufs
	install -D -m 644 README $(DESTDIR)/$(DOC_DIR)/README
	install -m 644 AUTHORS $(DESTDIR)/$(DOC_DIR)/AUTHORS
	install -m 644 COPYING $(DESTDIR)/$(DOC_DIR)/COPYING