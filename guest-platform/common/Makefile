HOST_CC ?= cc
HOST_TEST := /tmp/nativepipe-atomic-receive-test

.PHONY: test clean

test: tests/atomic_receive_test.c np.c np.h
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -pthread -I. \
		-o $(HOST_TEST) tests/atomic_receive_test.c np.c
	$(HOST_TEST)

clean:
	rm -f $(HOST_TEST)
