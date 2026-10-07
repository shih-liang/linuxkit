HOST_CC ?= cc
HOST_TEST := /tmp/nativepipe-atomic-receive-test
BOUNDS_TEST := /tmp/nativepipe-agent-bounds-test
FILE_RPC ?= ../../../nativepipe/common/file_rpc
TEST_CFLAGS := -std=c11 -Wall -Wextra -Werror -O2 -pthread -I. -I$(FILE_RPC)

.PHONY: test clean

test: tests/atomic_receive_test.c tests/agent_bounds_test.c np.c np.h $(FILE_RPC)/np_file_wire.c
	$(HOST_CC) $(TEST_CFLAGS) -o $(HOST_TEST) \
		tests/atomic_receive_test.c np.c $(FILE_RPC)/np_file_wire.c
	$(HOST_TEST)
	$(HOST_CC) $(TEST_CFLAGS) -o $(BOUNDS_TEST) \
		tests/agent_bounds_test.c $(FILE_RPC)/np_file_wire.c
	$(BOUNDS_TEST)

clean:
	rm -f $(HOST_TEST) $(BOUNDS_TEST)
