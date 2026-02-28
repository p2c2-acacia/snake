CC      = gcc

# ncurses paths – auto-detect pixi env, fall back to system defaults
NCURSES_PREFIX ?= $(shell [ -d "$(HOME)/.pixi/envs/ncurses" ] && echo "$(HOME)/.pixi/envs/ncurses" || echo "")
ifneq ($(NCURSES_PREFIX),)
  NCURSES_CFLAGS = -I$(NCURSES_PREFIX)/include
  NCURSES_LDFLAGS = -L$(NCURSES_PREFIX)/lib -Wl,-rpath,$(NCURSES_PREFIX)/lib -lncurses
else
  NCURSES_CFLAGS =
  NCURSES_LDFLAGS = -lncurses
endif

CFLAGS  = -Wall -Wextra -O2 -std=c11 $(NCURSES_CFLAGS)
LDFLAGS = $(NCURSES_LDFLAGS) -lm

TARGET  = snake
SRCDIR  = src
SRCS    = $(SRCDIR)/main.c $(SRCDIR)/game.c $(SRCDIR)/agent.c
OBJS    = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

$(SRCDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/game.h $(SRCDIR)/agent.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(SRCDIR)/*.o

.PHONY: all clean
