
CFLAGS=-W -Wall -pedantic -ansi -std=c99 -g

CFILES=../pcserver/diskimgs.c ../pcserver/log.c ../pcserver/terminal.c
HFILES=../pcserver/diskimgs.h ../pcserver/log.h ../common/petscii.h ../pcserver/terminal.h

reldump: reldump.c ${CFILES} ${HFILES}
	gcc ${CFLAGS} -o reldump -I../pcserver -I../common reldump.c ${CFILES} -lncurses

clean:
	rm -f reldump

