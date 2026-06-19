CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -g -Wall -Werror
LDFLAGS ?=
TARGET = aesdsocket

all: $(TARGET)

default: $(TARGET)

$(TARGET).o: $(TARGET).c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(TARGET).o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -pthread -lrt

clean:
	rm -f $(TARGET) *.o
