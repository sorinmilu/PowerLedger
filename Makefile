CC       := gcc
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -Werror \
            -Wstrict-prototypes -Wmissing-prototypes \
            -O2 -fno-plt -g \
            -Isrc/shared -Isrc/daemon
LDFLAGS  :=
DBUS_FLAGS := $(shell pkg-config --cflags --libs dbus-1 2>/dev/null)

BIN_DIR     := bin
DAEMON_BIN  := $(BIN_DIR)/power_ledger_d
LAYOUT_TEST := $(BIN_DIR)/layout_test

DAEMON_OBJS := src/daemon/main.o src/daemon/sysfs_poll.o src/daemon/binary_io.o
LAYOUT_OBJS := tests/layout_test.o src/daemon/binary_io.o

.PHONY: all test clean

all: $(DAEMON_BIN)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(DAEMON_BIN): $(DAEMON_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(DAEMON_OBJS) $(LDFLAGS) $(DBUS_FLAGS)

$(LAYOUT_TEST): $(LAYOUT_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(LAYOUT_OBJS) $(LDFLAGS)

src/daemon/main.o: src/daemon/main.c src/daemon/binary_io.h src/daemon/sysfs_poll.h \
                   src/shared/ledger_format.h
	$(CC) $(CFLAGS) -c -o $@ src/daemon/main.c

src/daemon/sysfs_poll.o: src/daemon/sysfs_poll.c src/daemon/sysfs_poll.h \
                         src/shared/ledger_format.h
	$(CC) $(CFLAGS) -c -o $@ src/daemon/sysfs_poll.c

src/daemon/binary_io.o: src/daemon/binary_io.c src/daemon/binary_io.h src/shared/ledger_format.h
	$(CC) $(CFLAGS) -c -o $@ src/daemon/binary_io.c

tests/layout_test.o: tests/layout_test.c src/daemon/binary_io.h src/shared/ledger_format.h
	$(CC) $(CFLAGS) -c -o $@ tests/layout_test.c

test: $(LAYOUT_TEST)
	./$(LAYOUT_TEST)

clean:
	rm -rf $(BIN_DIR) tmp
	rm -f src/daemon/*.o tests/*.o
