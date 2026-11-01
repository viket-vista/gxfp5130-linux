# SPDX-License-Identifier: GPL-2.0
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra

all: gxfp-tool

gxfp-tool: gxfp-tool.c ../gxfp5130_uapi.h
	$(CC) $(CFLAGS) -o $@ gxfp-tool.c

clean:
	rm -f gxfp-tool

.PHONY: all clean
