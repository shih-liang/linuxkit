# Static musl guestd. Shares ../common/np.c with bootstrap.
#
# Requires: zig (brew install zig)
# Version is baked into the ELF as the ASCII stamp "NPGV:<semver>" from ./VERSION.

ZIG ?= zig
COMMON := ../common/np.c
CONSOLE := console_config.c
CLOUD_INIT := cloud_init_config.c
ENVIRONMENT := environment_config.c
MDEV := mdev_config.c
SESSION_STACK := session_stack.c
VERSION := $(shell tr -d '[:space:]' < VERSION)
CFLAGS := -I../common -O2 -static -s -pthread -DNP_GUESTD_VERSION=\"$(VERSION)\"
HOST_CC ?= cc
HOST_TEST := /tmp/nativepipe-console-config-test
HOST_CLOUD_TEST := /tmp/nativepipe-cloud-init-config-test
HOST_ENVIRONMENT_TEST := /tmp/nativepipe-environment-config-test
HOST_MDEV_TEST := /tmp/nativepipe-mdev-config-test
HOST_SESSION_STACK_TEST := /tmp/nativepipe-session-stack-test

.PHONY: all clean aarch64 x86_64 test test-console test-cloud-init test-environment test-mdev test-session-stack

all: aarch64 x86_64

aarch64: dist/nativepipe-guestd-aarch64
x86_64: dist/nativepipe-guestd-x86_64

dist/nativepipe-guestd-aarch64: nativepipe-guestd.c $(CONSOLE) console_config.h $(CLOUD_INIT) cloud_init_config.h $(ENVIRONMENT) environment_config.h $(SESSION_STACK) session_stack.h $(COMMON) VERSION | dist
	$(ZIG) cc -target aarch64-linux-musl $(CFLAGS) -o $@ nativepipe-guestd.c $(CONSOLE) $(CLOUD_INIT) $(ENVIRONMENT) $(SESSION_STACK) $(COMMON) -lutil

dist/nativepipe-guestd-x86_64: nativepipe-guestd.c $(CONSOLE) console_config.h $(CLOUD_INIT) cloud_init_config.h $(ENVIRONMENT) environment_config.h $(SESSION_STACK) session_stack.h $(COMMON) VERSION | dist
	$(ZIG) cc -target x86_64-linux-musl $(CFLAGS) -o $@ nativepipe-guestd.c $(CONSOLE) $(CLOUD_INIT) $(ENVIRONMENT) $(SESSION_STACK) $(COMMON) -lutil

test: test-console test-cloud-init test-environment test-mdev test-session-stack

test-console: console_config.c console_config.h tests/console_config_test.c
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I. -o $(HOST_TEST) console_config.c tests/console_config_test.c
	$(HOST_TEST)

test-cloud-init: cloud_init_config.c cloud_init_config.h tests/cloud_init_config_test.c
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I. -o $(HOST_CLOUD_TEST) \
		cloud_init_config.c tests/cloud_init_config_test.c
	$(HOST_CLOUD_TEST)

test-environment: environment_config.c environment_config.h tests/environment_config_test.c
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I. -o $(HOST_ENVIRONMENT_TEST) \
		environment_config.c tests/environment_config_test.c
	$(HOST_ENVIRONMENT_TEST)

test-mdev: $(MDEV) mdev_config.h tests/mdev_config_test.c
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I. -o $(HOST_MDEV_TEST) \
		$(MDEV) tests/mdev_config_test.c
	$(HOST_MDEV_TEST)

test-session-stack: $(SESSION_STACK) session_stack.h tests/session_stack_test.c
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I. -o $(HOST_SESSION_STACK_TEST) \
		$(SESSION_STACK) tests/session_stack_test.c
	$(HOST_SESSION_STACK_TEST)

dist:
	mkdir -p dist

clean:
	rm -rf dist
	rm -f $(HOST_TEST) $(HOST_CLOUD_TEST)
	rm -f $(HOST_ENVIRONMENT_TEST)
	rm -f $(HOST_MDEV_TEST)
	rm -f $(HOST_SESSION_STACK_TEST)
