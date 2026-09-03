# Shared vsock + NPAG helpers, linked into bootstrap and guestd.
#
# Requires: zig (brew install zig)

ZIG ?= zig
COMMON := ../common/np.c
CFLAGS := -I../common -O2 -static -s

.PHONY: all clean aarch64 x86_64

all: aarch64 x86_64

aarch64: dist/nativepipe-bootstrap-aarch64
x86_64: dist/nativepipe-bootstrap-x86_64

dist/nativepipe-bootstrap-aarch64: nativepipe-bootstrap.c $(COMMON) | dist
	$(ZIG) cc -target aarch64-linux-musl $(CFLAGS) -o $@ nativepipe-bootstrap.c $(COMMON)

dist/nativepipe-bootstrap-x86_64: nativepipe-bootstrap.c $(COMMON) | dist
	$(ZIG) cc -target x86_64-linux-musl $(CFLAGS) -o $@ nativepipe-bootstrap.c $(COMMON)

dist:
	mkdir -p dist

clean:
	rm -rf dist
