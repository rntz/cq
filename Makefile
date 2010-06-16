include config.mk

.PHONY: all clean install

BINFILES=cq
LIBEXECFILES=cq cqd
MAN1FILES=cq.1
MANFILES=$(MAN1FILES)

all: $(LIBEXECFILES) $(MANFILES)

install: all
	install -d $(BIN)/ $(LIBEXEC)/ $(MAN)/man1/
	install -m 755 $(LIBEXECFILES) $(LIBEXEC)/
	install -m 644 $(MAN1FILES) $(MAN)/man1/
	ln -s $(addprefix $(LIBEXEC)/,$(BINFILES)) $(BIN)/

uninstall:
	rm -f $(addprefix $(BIN)/,$(BINFILES))
	rm -f $(addprefix $(LIBEXEC)/,$(LIBEXECFILES))
	rm -f $(addprefix $(MAN)/man1/,$(MAN1FILES))

clean:
	rm -f $(MANFILES)

%.1: %.pod
	pod2man -c '' -r '' -q '`' $< > $@
