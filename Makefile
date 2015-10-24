PREFIX?=	/usr/local
BINDIR?=	$(PREFIX)/bin
MANDIR?=	$(PREFIX)/man

CC?=		cc
CFLAGS+=	-Wall -O2 -g

all: vmtouch vmtouch.8

.PHONY: all install clean uninstall

vmtouch: vmtouch.c
	${CC} ${CFLAGS} -o vmtouch vmtouch.c

vmtouch.8: vmtouch.pod
	pod2man --section 8 vmtouch.pod > vmtouch.8

install: vmtouch vmtouch.8
	mkdir -p $(BINDIR)
	install -D -m0755 vmtouch $(BINDIR)/vmtouch
	mkdir -p $(MANDIR)/man8
	install -D -m 0644 vmtouch.8 $(MANDIR)/man8/vmtouch.8

clean:
	rm -f vmtouch vmtouch.8

uninstall:
	rm $(BINDIR)/vmtouch $(MANDIR)/man8/vmtouch.8
