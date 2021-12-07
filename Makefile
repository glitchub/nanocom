CFLAGS = -Wall -Werror -Os -s

# comment out to disable network support
CFLAGS += -DNETWORK

# comment out to disable telnet support
CFLAGS += -DTELNET

# comment out to disable high-character transliteration
CFLAGS += -DTRANSLIT

# comment out to disable shell command support
CFLAGS += -DSHELLCMD -lutil

# Comment in for older gcc
# CFLAGS += -std=gnu11

nanocom: nanocom.c Makefile; $(CC) $(CFLAGS) $< -o $@

.PHONY: clean
clean:; rm -f nanocom
