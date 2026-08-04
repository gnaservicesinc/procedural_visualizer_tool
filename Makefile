CC ?= cc
CFLAGS ?= -O3
CPPFLAGS ?=
WARNFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?= -lm

TARGET := render9
SOURCES := render9.c
HEADERS := stb_image_write.h

.PHONY: all run render check test debug clean

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(WARNFLAGS) $(CFLAGS) $(SOURCES) -o $@ $(LDFLAGS) $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

# Pass safe non-interactive overrides with ARGS, for example:
# make render ARGS="--width 640 --height 360 --frames 120 --output-dir preview"
render: $(TARGET)
	./$(TARGET) --render $(ARGS)

check test: $(TARGET)
	./$(TARGET) --self-test

debug: clean
	$(MAKE) CFLAGS="-O0 -g3" $(TARGET)

# Intentionally leaves all rendered PNG frames alone.
clean:
	$(RM) $(TARGET)
