CC       := gcc
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -Werror \
            -Wstrict-prototypes -Wmissing-prototypes \
            -O2 -fno-plt -g \
            -Isrc/shared -Isrc/daemon
LDFLAGS  :=
DBUS_FLAGS := $(shell pkg-config --cflags --libs dbus-1 2>/dev/null)

BIN_DIR     := bin
DAEMON_BIN  := $(BIN_DIR)/power_ledger_d
CLIENT_BIN  := $(BIN_DIR)/bat-time
LAYOUT_TEST := $(BIN_DIR)/layout_test

DAEMON_SRCS := src/daemon/binary_io.c
TEST_SRCS   := tests/layout_test.c $(DAEMON_SRCS)

DAEMON_OBJS := $(DAEMON_SRCS:.c=.o)
TEST_OBJS   := tests/layout_test.o src/daemon/binary_io.o

.PHONY: all test clean

all: $(LAYOUT_TEST)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(LAYOUT_TEST): $(TEST_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJS) $(LDFLAGS)

tests/layout_test.o: tests/layout_test.c src/daemon/binary_io.h src/shared/ledger_format.h
	$(CC) $(CFLAGS) -c -o $@ tests/layout_test.c

src/daemon/binary_io.o: src/daemon/binary_io.c src/daemon/binary_io.h src/shared/ledger_format.h
	$(CC) $(CFLAGS) -c -o $@ src/daemon/binary_io.c

test: $(LAYOUT_TEST)
	./$(LAYOUT_TEST)

clean:
	rm -rf $(BIN_DIR) tmp
	rm -f src/daemon/*.o tests/*.o
