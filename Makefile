.PHONY: default clean

CFLAGS = -Wall -Werror -Os -s -lutil

# Comment in for older gcc
# CFLAGS += -std=gnu11

default: nanocom
clean:; rm -f nanocom
