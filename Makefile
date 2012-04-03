all: build

build:
	gcc -Wall -O2 -g -o vmtouch vmtouch.c

install:
	install -D -m0755 vmtouch $(DESTDIR)/usr/sbin/vmtouch
	install -D -m0755 scripts/watch-vmtouch.pl $(DESTDIR)/usr/sbin/watch-vmtouch

clean:
	rm -f vmtouch
