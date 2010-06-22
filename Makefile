include config.mk

.PHONY: all clean install uninstall devinstall

BINFILES=cq
LIBEXECFILES=cq cqd
MAN1FILES=cq.1
MANFILES=$(MAN1FILES)

DISTFILES=Makefile config.mk cq.pod $(LIBEXECFILES)
DISTFILES+=INSTALL LICENSE NO-WARRANTY

all: $(LIBEXECFILES) $(MANFILES)

install: all
	install -d $(BIN)/ $(LIBEXEC)/ $(MAN)/man1/
	install -m 755 $(LIBEXECFILES) $(LIBEXEC)/
	install -m 644 $(MAN1FILES) $(MAN)/man1/
	ln -s $(addprefix $(LIBEXEC)/,$(BINFILES)) $(BIN)/

devinstall:
	install -d $(BIN)/
	ln -s $(addprefix $(PWD)/,$(BINFILES)) $(BIN)/

uninstall:
	rm -f $(addprefix $(BIN)/,$(BINFILES))
	rm -f $(addprefix $(LIBEXEC)/,$(LIBEXECFILES))
	rm -f $(addprefix $(MAN)/man1/,$(MAN1FILES))

clean:
	rm -f cq.tar.{gz,bz2} $(MANFILES)

cq.tar.bz2: $(DISTFILES)
	tar cjf $@ $^

cq.tar.gz: $(DISTFILES)
	tar czf $@ $^

%.1: %.pod
	pod2man -c '' -r '' -q '`' $< > $@
