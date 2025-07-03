CC=gcc
CFLAGS=-Wall -O3 -s
LDFLAGS=-lsensors

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
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(MANDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/
	install -m 644 $(MANPAGE) $(DESTDIR)$(MANDIR)/
	sudo mandb > /dev/null 2>&1
	@echo "Installation completed"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(MANDIR)/$(MANPAGE)
	@echo "Uninstallation completed"

clean:
	rm -f $(TARGET)