CC     ?= clang
CFLAGS  = -std=c11 -D_XOPEN_SOURCE=700 -Wall -Wextra -ggdb3 -pedantic
LDFLAGS =
SOURCES = $(shell echo *.c)
HEADERS = $(shell echo *.h)
OBJECTS = $(SOURCES:.c=.o)
HMVFS   = hmvfs

all: $(HMVFS)

$(HMVFS): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

.c.o:
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	rm -f $(OBJECTS)
	rm -f $(HMVFS)
