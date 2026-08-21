# Auto-generated Makefile for the modular Chard compiler split.
CC      = gcc
CFLAGS  = -O2 -Wall -Wno-unused-parameter -I.
SRCS    = $(shell find . -name '*.c')
OBJS    = $(SRCS:.c=.o)
TARGET  = chard

all: $(TARGET)
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
clean:
	find . -name '*.o' -delete
	rm -f $(TARGET)
.PHONY: all clean
