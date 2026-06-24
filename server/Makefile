CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -g -Wall -Werror
LDFLAGS ?=
TARGET = aesdsocket

USE_AESD_CHAR_DEVICE ?= 1

ifeq ($(USE_AESD_CHAR_DEVICE),1)
CFLAGS += -DUSE_AESD_CHAR_DEVICE=1
endif

all: $(TARGET)

default: $(TARGET)

$(TARGET).o: $(TARGET).c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(TARGET).o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -pthread -lrt

clean:
	rm -f $(TARGET) *.o
