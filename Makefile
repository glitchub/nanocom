CFLAGS = -Wall -Werror -s
LDFLAGS =

SRCS=nanocom.c queue.c

# comment in one of these
CFLAGS += -O3 # faster
# CFLAGS += -Os # smaller

# comment out to disable network support
CFLAGS += -DNETWORK

# comment out to disable telnet support
CFLAGS += -DTELNET
SRCS += telnet.c

# comment out to disable high-character transliteration
CFLAGS += -DTRANSLIT

# comment out to disable FX command support
CFLAGS += -DFXCMD
LDFLAGS += -lutil

# comment/uncomment as needed to make your gcc happy
# CFLAGS += -std=gnu11
CFLAGS += -Wno-unused-result

nanocom: ${SRCS} Makefile ; ${CC} ${CFLAGS} -o $@ ${SRCS} ${LDFLAGS}

.PHONY: clean
clean:; rm -f nanocom
