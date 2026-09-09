# Shared vsock + NPAG helpers, linked into bootstrap and guestd.
#
# Requires: zig (brew install zig)

ZIG ?= zig
FILE_RPC_DIR ?= $(abspath ../../../nativepipe/common/file_rpc)
COMMON := ../common/np.c $(FILE_RPC_DIR)/np_file_wire.c
HEADERS := ../common/np.h $(FILE_RPC_DIR)/np_file_rpc.h
CFLAGS := -I../common -I$(FILE_RPC_DIR) -O2 -static -s

.PHONY: all clean aarch64 x86_64

all: aarch64 x86_64

aarch64: dist/nativepipe-bootstrap-aarch64
x86_64: dist/nativepipe-bootstrap-x86_64

dist/nativepipe-bootstrap-aarch64: nativepipe-bootstrap.c $(COMMON) $(HEADERS) | dist
	$(ZIG) cc -target aarch64-linux-musl $(CFLAGS) -o $@ nativepipe-bootstrap.c $(COMMON)

dist/nativepipe-bootstrap-x86_64: nativepipe-bootstrap.c $(COMMON) $(HEADERS) | dist
	$(ZIG) cc -target x86_64-linux-musl $(CFLAGS) -o $@ nativepipe-bootstrap.c $(COMMON)

dist:
	mkdir -p dist

clean:
	rm -rf dist
