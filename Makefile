CC=gcc
CFLAGS=-Wall -O3 -s
LDFLAGS=-lsensors

TARGET=coreusage
SRC=main.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

install: $(TARGET)
	sudo cp $(TARGET) /usr/local/bin/

clean:
	rm -f $(TARGET) 