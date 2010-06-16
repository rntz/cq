CC=gcc
CFLAGS=-Wall -Wextra -Werror

.PHONY: clean

cqd: cqd.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f cqd
