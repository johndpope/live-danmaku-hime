# Fake Makefile, acturally calls build/Makefile

.PHONY: all install clean

all:
	make -C build all

install:
	make -C build install

clean:
	make -C build clean
