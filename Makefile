# this file is part of luufs.
#
# Copyright (c) 2014, 2015 Dima Krasner
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

PROG = luufs

CC ?= cc
CFLAGS ?= -Wall -pedantic
LDFLAGS ?=
DESTDIR ?= /
SBIN_DIR ?= sbin
DOC_DIR ?= usr/share/doc/$(PROG)
MAN_DIR ?= usr/share/man
HAVE_WAIVE ?= 0

CFLAGS += -std=gnu99 -D_GNU_SOURCE

FUSE_CFLAGS = $(shell pkg-config --cflags fuse)
ZLIB_CFLAGS = $(shell pkg-config --cflags zlib)
FUSE_LIBS = $(shell pkg-config --libs fuse)
ZLIB_LIBS = $(shell pkg-config --libs zlib)

ifeq (0,$(HAVE_WAIVE))
	LIBWAIVE_CFLAGS =
	LIBWAIVE_LIBS =
else
	CFLAGS += -DHAVE_WAIVE
	LIBWAIVE_CFLAGS = $(shell pkg-config --cflags libwaive)
	LIBWAIVE_LIBS = $(shell pkg-config --libs libwaive)
endif

SRCS = $(wildcard *.c)
OBJECTS = $(SRCS:.c=.o)
HEADERS = $(wildcard *.h)

%.o: %.c $(HEADERS)
	$(CC) -c -o $@ $< $(CFLAGS) $(FUSE_CFLAGS) $(ZLIB_CFLAGS) $(LIBWAIVE_CFLAGS)

$(PROG): $(OBJECTS)
	$(CC) -o $@ $^ $(LDFLAGS) $(FUSE_LIBS) $(ZLIB_LIBS) $(LIBWAIVE_LIBS)

test: $(PROG)
	sh test.sh

clean:
	rm -f $(PROG) $(OBJECTS)

install: $(PROG)
	install -D -m 755 $(PROG) $(DESTDIR)/$(SBIN_DIR)/$(PROG)
	install -D -m 644 $(PROG).8 $(DESTDIR)/$(MAN_DIR)/man8/$(PROG).8
	install -D -m 644 README $(DESTDIR)/$(DOC_DIR)/README
	install -m 644 AUTHORS $(DESTDIR)/$(DOC_DIR)/AUTHORS
	install -m 644 COPYING $(DESTDIR)/$(DOC_DIR)/COPYING
