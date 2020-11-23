.PHONY: default clean
CFLAGS = -Wall -Werror -Os -s
default: nanocom
clean:; rm nanocom
