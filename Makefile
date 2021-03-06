############################################################################
#
# Makefile for librtsp
#
# (C) Copyright 2008 to 2020 Hangzhou Xiongmai Technology Stock CO., LTD.
#
# Fri Jun 24 20:09:01 CST 2016		xcz		File created
#
############################################################################

CROSS	= arm-hisiv300-linux-
CC	= @echo " GCC	$@"; $(CROSS)gcc
CPP	= @echo " G++	$@"; $(CROSS)g++
LD	= @echo " LD	$@"; $(CROSS)ld
AR	= @echo " AR	$@"; $(CROSS)ar
STRIP	= @echo " STRIP $@"; $(CROSS)strip

CFLAGS = -std=gnu99

CFLAGS += -Iinclude 

TARGET = librtsp.a

SOURCE = $(wildcard src/*.c)

OBJS = $(patsubst src/%.c,src/%.o,$(SOURCE))

$(TARGET): $(OBJS)
	$(AR) -rv  $@ $^

.c.o:
	$(CC)  $(CFLAGS) -c -o $@ $<
	@echo $@
.cpp.o:
	$(CPP) $(CFLAGS) -c -o $@ $<


clean:
	rm -f $(TARGET) $(OBJS)

