CC=gcc
CPPFLAGS+=-D_POSIX_C_SOURCE=200809L
CFLAGS?=-O3 -std=c23
CFLAGS+=-Wall -Wextra -Wpedantic
LDLIBS+=-lsensors

# Verzeichnisse
PREFIX=/usr/local
BINDIR=$(PREFIX)/bin
MANDIR=$(PREFIX)/share/man/man1

TARGET=coreusage
SRC=main.c
MANPAGE=coreusage.1

# Phony Targets deklarieren
.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) -o $(TARGET) $(LDLIBS)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(MANDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/
	install -m 644 $(MANPAGE) $(DESTDIR)$(MANDIR)/
	@echo "Installed $(TARGET) to $(DESTDIR)$(BINDIR)"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(MANDIR)/$(MANPAGE)
	@echo "Uninstallation completed"

clean:
	rm -f $(TARGET)

debug: CFLAGS+=-Og -g -fsanitize=address,undefined
debug: clean $(TARGET)

update-mandb:
	mandb > /dev/null 2>&1 || true