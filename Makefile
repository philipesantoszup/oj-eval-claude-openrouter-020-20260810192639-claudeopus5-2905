CC = gcc
CFLAGS = -O2

.PHONY: all clean
all:
	$(CC) $(CFLAGS) -o code main.c buddy.c

clean:
	rm -f code
