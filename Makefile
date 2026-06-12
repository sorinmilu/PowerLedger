CC       := gcc
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -Werror \
            -Wstrict-prototypes -Wmissing-prototypes \
            -O2 -fno-plt -g \
            -Isrc/shared -Isrc/daemon -Itests
TEST_CFLAGS := $(CFLAGS) -DTEST_MODE
LDFLAGS  := -pthread
DBUS_FLAGS := $(shell pkg-config --cflags --libs dbus-1 2>/dev/null)

BIN_DIR      := bin
DAEMON_BIN   := $(BIN_DIR)/power_ledger_d
DAEMON_TEST_BIN := $(BIN_DIR)/power_ledger_d_test
BAT_TIME_BIN := $(BIN_DIR)/bat-time
LAYOUT_TEST  := $(BIN_DIR)/layout_test
TEST_RUNNER  := $(BIN_DIR)/power_test_suite
IPC_STRESS   := $(BIN_DIR)/ipc_stress
SEED_MOCK    := $(BIN_DIR)/seed_mock

DAEMON_OBJS := src/daemon/main.o src/daemon/sysfs_poll.o src/daemon/binary_io.o \
               src/daemon/dbus_handler.o src/daemon/ipc_socket.o src/shared/duration_format.o
DAEMON_TEST_OBJS := src/daemon/main_test.o tests/sysfs_poll_test.o \
                    src/daemon/binary_io_test.o src/daemon/dbus_handler_test.o \
                    src/daemon/ipc_socket_test.o src/shared/duration_format.o
CLIENT_OBJS := src/client/bat_time.o src/daemon/binary_io.o \
               src/daemon/sysfs_poll.o src/shared/duration_format.o
TEST_OBJS := tests/runner.o tests/hardware_mock.o tests/sysfs_poll_test.o \
             src/daemon/binary_io_test.o src/daemon/ipc_socket_test.o \
             src/shared/duration_format.o
LAYOUT_OBJS := tests/layout_test.o src/daemon/binary_io.o

VALGRIND := $(shell command -v valgrind 2>/dev/null)
IPC_SOCK := ./tmp/power_ledger.sock
VALGRIND_LEDGER := tmp/valgrind_ledger.bin

.PHONY: all test valgrind-test clean

all: $(DAEMON_BIN) $(BAT_TIME_BIN)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(DAEMON_BIN): $(DAEMON_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(DAEMON_OBJS) $(LDFLAGS) $(DBUS_FLAGS)

$(DAEMON_TEST_BIN): $(DAEMON_TEST_OBJS) | $(BIN_DIR)
	$(CC) $(TEST_CFLAGS) -o $@ $(DAEMON_TEST_OBJS) $(LDFLAGS) $(DBUS_FLAGS)

$(BAT_TIME_BIN): $(CLIENT_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(CLIENT_OBJS) -lm

$(LAYOUT_TEST): $(LAYOUT_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(LAYOUT_OBJS) $(LDFLAGS)

$(TEST_RUNNER): $(TEST_OBJS) | $(BIN_DIR)
	$(CC) $(TEST_CFLAGS) -o $@ $(TEST_OBJS) -lm $(LDFLAGS)

$(IPC_STRESS): tests/ipc_stress.o | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ tests/ipc_stress.o $(LDFLAGS)

$(SEED_MOCK): tests/seed_mock.o tests/hardware_mock.o | $(BIN_DIR)
	$(CC) $(TEST_CFLAGS) -o $@ tests/seed_mock.o tests/hardware_mock.o $(LDFLAGS)

test: $(LAYOUT_TEST) $(TEST_RUNNER) $(BAT_TIME_BIN)
	./$(LAYOUT_TEST)
	./$(BAT_TIME_BIN) --self-test
	./$(TEST_RUNNER)

valgrind-test: $(DAEMON_TEST_BIN) $(IPC_STRESS) $(SEED_MOCK)
	@if [ -z "$(VALGRIND)" ]; then \
		echo "valgrind not installed; skipping leak profiler"; \
		exit 0; \
	fi
	@rm -rf tmp mock_sys
	@mkdir -p tmp
	@./$(SEED_MOCK)
	@$(VALGRIND) --leak-check=full --error-exitcode=1 --track-fds=yes \
		./$(DAEMON_TEST_BIN) -f $(VALGRIND_LEDGER) & \
	DAEMON_PID=$$!; \
	READY=0; \
	for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 \
		21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 \
		41 42 43 44 45 46 47 48 49 50; do \
		if [ -S $(IPC_SOCK) ]; then READY=1; break; fi; \
		sleep 0.1; \
	done; \
	if [ "$$READY" -ne 1 ]; then \
		echo "valgrind-test failed: IPC socket not ready"; \
		kill -TERM $$DAEMON_PID 2>/dev/null || true; \
		wait $$DAEMON_PID 2>/dev/null || true; \
		rm -rf tmp mock_sys; \
		exit 1; \
	fi; \
	./$(IPC_STRESS) 50 $(IPC_SOCK); \
	STRESS_RC=$$?; \
	if [ $$STRESS_RC -ne 0 ]; then \
		kill -TERM $$DAEMON_PID 2>/dev/null || true; \
		wait $$DAEMON_PID 2>/dev/null || true; \
		rm -rf tmp mock_sys; \
		exit $$STRESS_RC; \
	fi; \
	kill -TERM $$DAEMON_PID; \
	wait $$DAEMON_PID; \
	VALGRIND_RC=$$?; \
	rm -rf tmp mock_sys; \
	if [ $$VALGRIND_RC -ne 0 ]; then \
		echo "valgrind leak profiler failed (exit $$VALGRIND_RC)"; \
		exit $$VALGRIND_RC; \
	fi; \
	echo "valgrind leak profiler passed: 50 IPC queries, no leaks"

src/daemon/main.o: src/daemon/main.c src/daemon/binary_io.h src/daemon/dbus_handler.h \
                   src/daemon/ipc_socket.h src/daemon/sysfs_poll.h src/shared/ledger_format.h
	$(CC) $(CFLAGS) -c -o $@ src/daemon/main.c

src/daemon/main_test.o: src/daemon/main.c src/daemon/binary_io.h src/daemon/dbus_handler.h \
                        src/daemon/ipc_socket.h src/daemon/sysfs_poll.h src/shared/ledger_format.h
	$(CC) $(TEST_CFLAGS) -c -o $@ src/daemon/main.c

src/daemon/ipc_socket.o: src/daemon/ipc_socket.c src/daemon/ipc_socket.h \
                         src/daemon/binary_io.h src/daemon/sysfs_poll.h \
                         src/shared/duration_format.h src/shared/ledger_format.h
	$(CC) $(CFLAGS) -c -o $@ src/daemon/ipc_socket.c

src/daemon/ipc_socket_test.o: src/daemon/ipc_socket.c src/daemon/ipc_socket.h \
                              src/daemon/binary_io.h src/daemon/sysfs_poll.h \
                              src/shared/duration_format.h src/shared/ledger_format.h
	$(CC) $(TEST_CFLAGS) -c -o $@ src/daemon/ipc_socket.c

src/shared/duration_format.o: src/shared/duration_format.c src/shared/duration_format.h
	$(CC) $(CFLAGS) -c -o $@ src/shared/duration_format.c

src/client/bat_time.o: src/client/bat_time.c src/shared/ledger_format.h
	$(CC) $(CFLAGS) -Isrc/shared -c -o $@ src/client/bat_time.c

src/daemon/dbus_handler.o: src/daemon/dbus_handler.c src/daemon/dbus_handler.h \
                           src/daemon/binary_io.h src/daemon/sysfs_poll.h \
                           src/shared/ledger_format.h
	$(CC) $(CFLAGS) $(DBUS_FLAGS) -c -o $@ src/daemon/dbus_handler.c

src/daemon/dbus_handler_test.o: src/daemon/dbus_handler.c src/daemon/dbus_handler.h \
                                src/daemon/binary_io.h src/daemon/sysfs_poll.h \
                                src/shared/ledger_format.h
	$(CC) $(TEST_CFLAGS) $(DBUS_FLAGS) -c -o $@ src/daemon/dbus_handler.c

tests/sysfs_poll_test.o: src/daemon/sysfs_poll.c src/daemon/sysfs_poll.h \
                         src/shared/ledger_format.h
	$(CC) $(TEST_CFLAGS) -c -o $@ src/daemon/sysfs_poll.c

src/daemon/sysfs_poll.o: src/daemon/sysfs_poll.c src/daemon/sysfs_poll.h \
                         src/shared/ledger_format.h
	$(CC) $(CFLAGS) -c -o $@ src/daemon/sysfs_poll.c

src/daemon/binary_io.o: src/daemon/binary_io.c src/daemon/binary_io.h src/shared/ledger_format.h
	$(CC) $(CFLAGS) -c -o $@ src/daemon/binary_io.c

src/daemon/binary_io_test.o: src/daemon/binary_io.c src/daemon/binary_io.h \
                             src/shared/ledger_format.h
	$(CC) $(TEST_CFLAGS) -c -o $@ src/daemon/binary_io.c

tests/layout_test.o: tests/layout_test.c src/daemon/binary_io.h src/shared/ledger_format.h
	$(CC) $(CFLAGS) -c -o $@ tests/layout_test.c

tests/runner.o: tests/runner.c tests/hardware_mock.h src/daemon/binary_io.h \
                src/daemon/ipc_socket.h src/daemon/sysfs_poll.h \
                src/shared/duration_format.h src/shared/ledger_format.h
	$(CC) $(TEST_CFLAGS) -c -o $@ tests/runner.c

tests/hardware_mock.o: tests/hardware_mock.c tests/hardware_mock.h
	$(CC) $(TEST_CFLAGS) -c -o $@ tests/hardware_mock.c

tests/ipc_stress.o: tests/ipc_stress.c
	$(CC) $(CFLAGS) -c -o $@ tests/ipc_stress.c

tests/seed_mock.o: tests/seed_mock.c tests/hardware_mock.h
	$(CC) $(TEST_CFLAGS) -c -o $@ tests/seed_mock.c

clean:
	rm -rf $(BIN_DIR) mock_sys tmp
	rm -f src/daemon/*.o src/client/*.o tests/*.o
