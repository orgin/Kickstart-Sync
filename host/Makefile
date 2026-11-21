# KickSync - host side tool.  Needs a C compiler and zlib.
#   make            build ./KickSync
#   make test       run the tests against kickserv_mock.py (needs python3, zip)
#   make CC=x86_64-w64-mingw32-gcc EXE=.exe LDLIBS="-lz -lws2_32"   (Windows)

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99
LDLIBS  ?= -lz
EXE     ?=

KickSync$(EXE): main.c kicksync.c kicksync.h
	$(CC) $(CFLAGS) -o $@ main.c kicksync.c $(LDLIBS)

test: KickSync$(EXE)
	./test.sh

clean:
	rm -f KickSync KickSync.exe

.PHONY: test clean
