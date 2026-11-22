# Cross compile:  make
# Via Docker:     make docker          (DOCKER="sudo docker" if needed)
# Release LHA:    ../tools/make_lha.sh (also builds KickSync for Linux)
CC      = ppc-amigaos-gcc
CFLAGS  = -O2 -Wall -W -D__USE_INLINE__
LDLIBS  = -lauto
IMAGE   = walkero/amigagccondocker:os4-gcc15-3.7.0
DOCKER ?= docker

KickServ: kickserv.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

docker:
	$(DOCKER) run --rm -u "$$(id -u):$$(id -g)" -v "$(CURDIR)":/src -w /src $(IMAGE) make KickServ

clean:
	rm -f KickServ

.PHONY: docker clean
