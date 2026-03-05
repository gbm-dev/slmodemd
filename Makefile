FORCE_32BIT ?= 1

# Detect architecture and force 32-bit compiler for slmodemd on 64-bit hosts.
ARCH := $(shell uname -m)
ifeq ($(FORCE_32BIT),1)
ifeq ($(ARCH),x86_64)
    ifeq ($(origin CC), default)
        CC := gcc -m32
    endif
else
    ifeq ($(origin CC), default)
        CC := gcc
    endif
endif
else
    ifeq ($(origin CC), default)
        CC := gcc
    endif
endif

export CC

all: slmodemd

slmodemd:
	$(MAKE) -C slmodemd

clean:
	$(MAKE) -C slmodemd clean

realclean: clean
	@true

.PHONY: all clean realclean slmodemd
