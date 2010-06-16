.PHONY: all clean

all: cq.1

%.1: %.pod
	pod2man -c '' -r '' -q '`' $< > $@

clean:
	rm -f cq.1
