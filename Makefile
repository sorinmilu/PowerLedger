CC       := gcc
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -Werror \
            -Wstrict-prototypes -Wmissing-prototypes \
            -O2 -fno-plt -g \
            -Isrc/shared -Isrc/daemon -Itests
TEST_CFLAGS := $(CFLAGS) -DTEST_MODE
LDFLAGS  :=
DBUS_FLAGS := $(shell pkg-config --cflags --libs dbus-1 2>/dev/null)

BIN_DIR     := bin
LAYOUT_TEST := $(BIN_DIR)/layout_test
TEST_RUNNER := $(BIN_DIR)/power_test_suite

TEST_OBJS := tests/runner.o tests/hardware_mock.o tests/sysfs_poll_test.o \
             src/daemon/binary_io.o
LAYOUT_OBJS := tests/layout_test.o src/daemon/binary_io.o

.PHONY: all test clean

all: $(LAYOUT_TEST)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(LAYOUT_TEST): $(LAYOUT_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(LAYOUT_OBJS) $(LDFLAGS)

$(TEST_RUNNER): $(TEST_OBJS) | $(BIN_DIR)
	$(CC) $(TEST_CFLAGS) -o $@ $(TEST_OBJS) $(LDFLAGS)

tests/sysfs_poll_test.o: src/daemon/sysfs_poll.c src/daemon/sysfs_poll.h \
                         src/shared/ledger_format.h
	$(CC) $(TEST_CFLAGS) -c -o $@ src/daemon/sysfs_poll.c

src/daemon/sysfs_poll.o: src/daemon/sysfs_poll.c src/daemon/sysfs_poll.h \
                         src/shared/ledger_format.h
	$(CC) $(CFLAGS) -c -o $@ src/daemon/sysfs_poll.c

src/daemon/binary_io.o: src/daemon/binary_io.c src/daemon/binary_io.h src/shared/ledger_format.h
	$(CC) $(CFLAGS) -c -o $@ src/daemon/binary_io.c

tests/layout_test.o: tests/layout_test.c src/daemon/binary_io.h src/shared/ledger_format.h
	$(CC) $(CFLAGS) -c -o $@ tests/layout_test.c

tests/runner.o: tests/runner.c tests/hardware_mock.h src/daemon/binary_io.h \
                src/daemon/sysfs_poll.h src/shared/ledger_format.h
	$(CC) $(TEST_CFLAGS) -c -o $@ tests/runner.c

tests/hardware_mock.o: tests/hardware_mock.c tests/hardware_mock.h
	$(CC) $(TEST_CFLAGS) -c -o $@ tests/hardware_mock.c

test: $(LAYOUT_TEST) $(TEST_RUNNER)
	./$(LAYOUT_TEST)
	./$(TEST_RUNNER)

clean:
	rm -rf $(BIN_DIR) mock_sys tmp
	rm -f src/daemon/*.o tests/*.o
