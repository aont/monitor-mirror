CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS
LDFLAGS ?=
LDLIBS ?= -ld3d11 -ldxgi -ldxguid -luser32 -lgdi32 -ld3dcompiler

SRC_DIR := src
BIN_DIR := bin
TARGET := $(BIN_DIR)/monitor_mirror.exe
SOURCES := $(SRC_DIR)/main.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SOURCES) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(SOURCES) -o $@ $(LDFLAGS) $(LDLIBS)

$(BIN_DIR):
	mkdir -p $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BIN_DIR)
