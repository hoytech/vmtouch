PREFIX?=	/usr/local
BINDIR?=	$(PREFIX)/bin
MANDIR?=	$(PREFIX)/man

CC?=		cc
CFLAGS+=	-Wall -O2 -g

all: build

build:
	${CC} ${CFLAGS} -o vmtouch vmtouch.c
	pod2man --section 8 vmtouch.pod > vmtouch.8

install: build
	mkdir -p $(BINDIR)
	install -D -m0755 vmtouch $(BINDIR)/vmtouch
	mkdir -p $(MANDIR)/man8
	install -D -m 0644 vmtouch.8 $(MANDIR)/man8/vmtouch.8

clean:
	rm -f vmtouch
	rm -f vmtouch.8

uninstall:
	rm $(BINDIR)/vmtouch
	rm $(MANDIR)/man8/vmtouch.8
