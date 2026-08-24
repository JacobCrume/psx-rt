CC       ?= cc
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS   += `sdl2-config --cflags`
LDFLAGS =
LIBS    = `sdl2-config --libs`

SRCS = src/main.c src/cpu.c src/memory.c src/gpu.c src/dma.c src/timers.c src/cdrom.c src/hle.c
OBJS = $(SRCS:.c=.o)

oxpsx: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.c src/psx.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f oxpsx $(OBJS)

.PHONY: clean
