CFLAGS = -Wall -Werror -s
LDFLAGS =

# comment in one of these
CFLAGS += -O3 # faster
# CFLAGS += -Os # smaller

# comment out to disable network support
CFLAGS += -DNETWORK

# comment out to disable telnet support
CFLAGS += -DTELNET

# comment out to disable high-character transliteration
CFLAGS += -DTRANSLIT

# comment out to disable shell command support
CFLAGS += -DSHELLCMD
LDFLAGS += -lutil

# comment/uncomment as needed to make your gcc happy
# CFLAGS += -std=gnu11
CFLAGS += -Wno-unused-result

nanocom: nanocom.c Makefile; $(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

.PHONY: clean
clean:; rm -f nanocom
