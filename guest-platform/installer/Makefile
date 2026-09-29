CC ?= cc
PYTHON ?= python3
PKG_CONFIG ?= pkg-config
BUILD ?= ../build/installer
FILE_RPC_DIR ?= $(abspath ../../../nativepipe/common/file_rpc)
CFLAGS ?= -O2 -g
CHECK_FLAGS := -std=c11 -Wall -Wextra -Werror -UNDEBUG
INCLUDES := -I. -I../common -I$(FILE_RPC_DIR)
ARCHIVE_FLAGS = $(shell $(PKG_CONFIG) --cflags libarchive json-c libcrypto)
ARCHIVE_LIBS = $(shell $(PKG_CONFIG) --libs libarchive json-c libcrypto)
COMMON := ../common/np.c $(FILE_RPC_DIR)/np_file_wire.c
ROOT := ../common/root.c
ROOT_HEADER := ../common/root.h

.PHONY: all test test-root

all: $(BUILD)/nativepipe-install

$(BUILD)/nativepipe-install: nativepipe-install.c configure.c software.c amd64.c install.h distro.c distro.h $(ROOT) $(ROOT_HEADER) rootfs.c rootfs.h ../common/os_release.c ../common/os_release.h ../common/rosetta.c ../common/rosetta.h $(COMMON) $(FILE_RPC_DIR)/np_file_server.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CHECK_FLAGS) $(INCLUDES) $(ARCHIVE_FLAGS) \
		nativepipe-install.c configure.c software.c amd64.c distro.c $(ROOT) rootfs.c \
		../common/os_release.c ../common/rosetta.c $(COMMON) $(FILE_RPC_DIR)/np_file_server.c \
		$(LDFLAGS) $(ARCHIVE_LIBS) -o $@

test: $(BUILD)/policy-test $(BUILD)/rootfs-extract
	$(BUILD)/policy-test
	$(PYTHON) tests/rootfs_test.py $(BUILD)/rootfs-extract

$(BUILD)/policy-test: tests/policy_test.c distro.c distro.h ../common/rosetta.c ../common/rosetta.h ../common/os_release.c ../common/os_release.h $(COMMON)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CHECK_FLAGS) $(INCLUDES) tests/policy_test.c distro.c \
		../common/rosetta.c ../common/os_release.c $(COMMON) -o $@

$(BUILD)/rootfs-extract: tests/extract.c rootfs.c rootfs.h distro.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CHECK_FLAGS) $(INCLUDES) $(ARCHIVE_FLAGS) \
		tests/extract.c rootfs.c $(ARCHIVE_LIBS) -o $@

# Run only as root in a disposable Linux VM. The test binary is static so its
# empty test root needs no distribution libraries or external shell.
$(BUILD)/root-test: tests/root_test.c $(ROOT) $(ROOT_HEADER) $(COMMON) $(FILE_RPC_DIR)/np_file_server.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CHECK_FLAGS) $(INCLUDES) -static tests/root_test.c $(ROOT) \
		$(COMMON) $(FILE_RPC_DIR)/np_file_server.c -o $@

$(BUILD)/package-test: tests/package_test.c configure.c software.c ../common/os_release.c install.h distro.c distro.h $(ROOT) $(ROOT_HEADER) $(COMMON) $(FILE_RPC_DIR)/np_file_server.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CHECK_FLAGS) $(INCLUDES) -static tests/package_test.c configure.c software.c ../common/os_release.c distro.c $(ROOT) \
		$(COMMON) $(FILE_RPC_DIR)/np_file_server.c -o $@

test-root: $(BUILD)/root-test $(BUILD)/package-test $(BUILD)/nativepipe-install
	$(BUILD)/root-test
	$(BUILD)/package-test
	$(PYTHON) tests/preflight_test.py $(BUILD)/nativepipe-install

$(BUILD)/run-root: tests/run-root.c $(ROOT) $(ROOT_HEADER) $(COMMON) $(FILE_RPC_DIR)/np_file_server.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CHECK_FLAGS) -D_DEFAULT_SOURCE $(INCLUDES) tests/run-root.c $(ROOT) \
		$(COMMON) $(FILE_RPC_DIR)/np_file_server.c -o $@
