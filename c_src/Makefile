# Copyright 2012-2016 Tail-f Systems AB
#
# See the file "LICENSE" for information on usage and redistribution
# of this file, and for a DISCLAIMER OF ALL WARRANTIES.

include ../include.mk

PROG = ../priv/bin/runpty

all: $(PROG)

$(PROG): runpty.c
	$(CC) -o $@ $(CFLAGS) $(LDFLAGS) $<

clean:
	rm -rf $(PROG) ../priv/bin/runpty.dSYM core* *~

install:
