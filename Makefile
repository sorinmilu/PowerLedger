CC       := gcc
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -Werror \
            -Wstrict-prototypes -Wmissing-prototypes \
            -O2 -fno-plt -g \
            -Isrc/shared -Isrc/daemon
LDFLAGS  :=
DBUS_FLAGS := $(shell pkg-config --cflags --libs dbus-1 2>/dev/null)

BIN_DIR     := bin
LAYOUT_TEST := $(BIN_DIR)/layout_test

LAYOUT_OBJS := tests/layout_test.o src/daemon/binary_io.o

.PHONY: all test clean compile-sysfs

all: $(LAYOUT_TEST)

compile-sysfs: src/daemon/sysfs_poll.o

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(LAYOUT_TEST): $(LAYOUT_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(LAYOUT_OBJS) $(LDFLAGS)

tests/layout_test.o: tests/layout_test.c src/daemon/binary_io.h src/shared/ledger_format.h
	$(CC) $(CFLAGS) -c -o $@ tests/layout_test.c

src/daemon/binary_io.o: src/daemon/binary_io.c src/daemon/binary_io.h src/shared/ledger_format.h
	$(CC) $(CFLAGS) -c -o $@ src/daemon/binary_io.c

src/daemon/sysfs_poll.o: src/daemon/sysfs_poll.c src/daemon/sysfs_poll.h \
                         src/shared/ledger_format.h
	$(CC) $(CFLAGS) -c -o $@ src/daemon/sysfs_poll.c

test: $(LAYOUT_TEST)
	./$(LAYOUT_TEST)

clean:
	rm -rf $(BIN_DIR) tmp
	rm -f src/daemon/*.o tests/*.o
