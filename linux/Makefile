# SPDX-License-Identifier: GPL-2.0
obj-m += gxfp5130.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

test:
	cc -O2 -Wall -Wextra -o tests/test_proto tests/test_proto.c
	./tests/test_proto

.PHONY: all clean test
