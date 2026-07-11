CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?= -Iinclude -Isample

BUILD_DIR := build

LIB_SRC := src/window_skin.c
DEMO_SRC := sample/main.c sample/demo_backend.c

TARGET := $(BUILD_DIR)/window_skin_demo

all: $(TARGET)

$(TARGET): $(LIB_SRC) $(DEMO_SRC) include/window_skin.h sample/demo_backend.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LIB_SRC) $(DEMO_SRC) -lm -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all run clean
