.PHONY: default clean
CFLAGS = -Wall -Werror -Os -s
# old gcc may need this
# CFLAGS += -std=gnu11
default: nanocom
clean:; rm nanocom
