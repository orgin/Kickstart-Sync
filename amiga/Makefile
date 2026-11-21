# Cross compile:  make
# Via Docker:     make docker
CC      = ppc-amigaos-gcc
CFLAGS  = -O2 -Wall -W -D__USE_INLINE__
LDLIBS  = -lauto
IMAGE   = walkero/amigagccondocker:os4-gcc11

KickServ: kickserv.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

docker:
	docker run --rm -v "$(CURDIR)":/src -w /src $(IMAGE) make KickServ

clean:
	rm -f KickServ

.PHONY: docker clean
