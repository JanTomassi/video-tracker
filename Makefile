# vt - video tracking
# See LICENSE file for copyright and license details.

# include config.mk

INC:=$(shell pkg-config --cflags libavformat libavcodec libswresample libswscale libavutil sdl2)
LDFLAGS:=$(shell pkg-config --libs libavformat libavcodec libswresample libswscale libavutil sdl2) -lm
SRC = vt.c
OBJ = ${SRC:.c=.o}

all: vt TAGS

format: .clang-format
	clang-format -i ${SRC} ${INC}

TAGS: ${SRC}
#	ctags -eR ${SRC} "/usr/include" $(shell echo ${INC} | sed "s/-I//g")

%.o: %.c
	clang-format -i $< 
	${CC} -c -ggdb -O0 ${CFLAGS} $<

vt: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

clean:
	rm -f vt ${OBJ} dwm-${VERSION}.tar.gz

dist: clean
	mkdir -p vt-${VERSION}
	cp -R ${INC} ${SRC} vt-${VERSION}
	tar -cf vt-${VERSION}.tar vt-${VERSION}
	gzip vt-${VERSION}.tar
	rm -rf vt-${VERSION}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f dwm ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/vt
#	mkdir -p ${DESTDIR}${MANPREFIX}/man1
#	sed "s/VERSION/${VERSION}/g" < dwm.1 > ${DESTDIR}${MANPREFIX}/man1/dwm.1
#	chmod 644 ${DESTDIR}${MANPREFIX}/man1/dwm.1

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/vt ${DESTDIR}${MANPREFIX}/man1/vt.1

.PHONY: all clean dist install uninstall format
