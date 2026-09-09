ZIG ?= zig
HOST_CC ?= cc
PYTHON ?= python3
ZIG_GLOBAL_CACHE_DIR ?= $(CURDIR)/build/zig-global-cache
ZIG_LOCAL_CACHE_DIR ?= $(CURDIR)/build/zig-local-cache
export ZIG_GLOBAL_CACHE_DIR ZIG_LOCAL_CACHE_DIR

VERSION := $(shell tr -d '[:space:]' < guestd/VERSION)
FILE_RPC_DIR ?= $(abspath ../../nativepipe/common/file_rpc)
COMMON_SOURCE := common/np.c $(FILE_RPC_DIR)/np_file_wire.c
COMMON_HEADER := common/np.h $(FILE_RPC_DIR)/np_file_rpc.h
GUESTD_SOURCES := \
	guestd/nativepipe-guestd.c guestd/exec_stream.c \
	guestd/console_config.c \
	guestd/cloud_init_config.c \
	guestd/environment_config.c \
	guestd/session_stack.c \
	guestd/shared_folders.c \
	$(FILE_RPC_DIR)/np_file_server.c $(FILE_RPC_DIR)/np_file_service.c
GUESTD_HEADERS := \
	guestd/console_config.h guestd/exec_stream.h \
	guestd/cloud_init_config.h \
	guestd/environment_config.h \
	guestd/session_stack.h \
	guestd/shared_folders.h
TARGET_CFLAGS := -std=c11 -Icommon -I$(FILE_RPC_DIR) -O2 -static -s
GUESTD_CFLAGS := $(TARGET_CFLAGS) -pthread -DNP_GUESTD_VERSION=\"$(VERSION)\"
HOST_CFLAGS := -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O2

.PHONY: all build aarch64 x86_64 test test-license clean

all: build
build: aarch64 x86_64
aarch64: build/aarch64/nativepipe-bootstrap build/aarch64/nativepipe-guestd
x86_64: build/x86_64/nativepipe-bootstrap build/x86_64/nativepipe-guestd

build/aarch64/nativepipe-bootstrap: bootstrap/nativepipe-bootstrap.c $(COMMON_SOURCE) $(COMMON_HEADER)
	@mkdir -p $(@D)
	$(ZIG) cc -target aarch64-linux-musl $(TARGET_CFLAGS) -o $@ \
		bootstrap/nativepipe-bootstrap.c $(COMMON_SOURCE)

build/x86_64/nativepipe-bootstrap: bootstrap/nativepipe-bootstrap.c $(COMMON_SOURCE) $(COMMON_HEADER)
	@mkdir -p $(@D)
	$(ZIG) cc -target x86_64-linux-musl $(TARGET_CFLAGS) -o $@ \
		bootstrap/nativepipe-bootstrap.c $(COMMON_SOURCE)

build/aarch64/nativepipe-guestd: $(GUESTD_SOURCES) $(GUESTD_HEADERS) $(COMMON_SOURCE) $(COMMON_HEADER) guestd/VERSION
	@mkdir -p $(@D)
	$(ZIG) cc -target aarch64-linux-musl $(GUESTD_CFLAGS) -o $@ \
		$(GUESTD_SOURCES) $(COMMON_SOURCE) -lutil

build/x86_64/nativepipe-guestd: $(GUESTD_SOURCES) $(GUESTD_HEADERS) $(COMMON_SOURCE) $(COMMON_HEADER) guestd/VERSION
	@mkdir -p $(@D)
	$(ZIG) cc -target x86_64-linux-musl $(GUESTD_CFLAGS) -o $@ \
		$(GUESTD_SOURCES) $(COMMON_SOURCE) -lutil

build/host-tests/atomic-receive: common/tests/atomic_receive_test.c $(COMMON_SOURCE) $(COMMON_HEADER)
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -pthread -Icommon -I$(FILE_RPC_DIR) -o $@ $< $(COMMON_SOURCE)

build/host-tests/console-config: guestd/tests/console_config_test.c guestd/console_config.c guestd/console_config.h
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -Iguestd -o $@ guestd/console_config.c $<

build/host-tests/cloud-init-config: guestd/tests/cloud_init_config_test.c guestd/cloud_init_config.c guestd/cloud_init_config.h
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -Iguestd -o $@ guestd/cloud_init_config.c $<

build/host-tests/environment-config: guestd/tests/environment_config_test.c guestd/environment_config.c guestd/environment_config.h guestd/console_config.h
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -Iguestd -o $@ guestd/environment_config.c $<

build/host-tests/mdev-config: guestd/tests/mdev_config_test.c guestd/mdev_config.c guestd/mdev_config.h
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -Iguestd -o $@ guestd/mdev_config.c $<

build/host-tests/session-stack: guestd/tests/session_stack_test.c guestd/session_stack.c guestd/session_stack.h
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -Iguestd -o $@ guestd/session_stack.c $<

build/host-tests/exec-stream: guestd/exec_stream.c guestd/exec_stream.h guestd/tests/exec_stream_test.c
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -pthread -Iguestd guestd/tests/exec_stream_test.c guestd/exec_stream.c -o $@

test: build/host-tests/exec-stream \
	build/host-tests/atomic-receive \
	build/host-tests/console-config \
	build/host-tests/cloud-init-config \
	build/host-tests/environment-config \
	build/host-tests/mdev-config \
	build/host-tests/session-stack
	$(MAKE) -C $(FILE_RPC_DIR) test
	@for test_binary in $^; do "$$test_binary" || exit $$?; done

test-license:
	$(PYTHON) scripts/check-license-inventory.py

clean:
	rm -rf build dist
