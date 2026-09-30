CC ?= cc
BUILD ?= build
CFLAGS ?= -O2
CHECK_FLAGS := -std=c11 -Wall -Wextra -Werror

.PHONY: all test
all: $(BUILD)/libnativepipe-rosetta.so

# No libc dependency: resolve the small common ABI against the calling
# process's libc, without loading a second libc into systemd.
$(BUILD)/libnativepipe-rosetta.so: faccessat.c close_range.c ../common/fd_range.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CHECK_FLAGS) -shared -fPIC -nostdlib -Wl,-z,relro,-z,now faccessat.c close_range.c -o $@

$(BUILD)/old-libc.so: tests/old-libc.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CHECK_FLAGS) -shared -fPIC -nostdlib $< -o $@

$(BUILD)/faccessat-test: tests/faccessat-test.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CHECK_FLAGS) -UNDEBUG $< -o $@

test: $(BUILD)/libnativepipe-rosetta.so $(BUILD)/old-libc.so $(BUILD)/faccessat-test
	LD_PRELOAD='$(abspath $(BUILD)/libnativepipe-rosetta.so):$(abspath $(BUILD)/old-libc.so)' $(BUILD)/faccessat-test
