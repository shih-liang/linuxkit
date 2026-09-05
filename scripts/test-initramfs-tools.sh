#!/bin/sh
# Run the actual ARM64 recovery tools, not the runner's full-featured /bin/sh.
set -eu

root=${1:?assembled initramfs directory is required}
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
busybox="$root/bin/busybox"
test -x "$busybox"

run_busybox()
{
	qemu-aarch64 "$busybox" "$@"
}

for adapter in "$repo"/adapters/*.sh; do
	run_busybox sh -n "$adapter"
done

# These are used by the partition-device wait loops and the DHCP hook.
run_busybox sh -ec 'test "$((2147483647 + 1))" = 2147483648; sleep 0.01'
run_busybox sleep 0.01
run_busybox sh "$repo/adapters/udhcpc.sh" --self-test

# Source the complete shared adapter without touching a disk or mountpoint.
NP_SOURCE_PATH=/nonexistent/source NP_TARGET_ROOT=/ \
	qemu-aarch64 "$busybox" sh -ec '
		. "$1"
		[ "$(partition_path /dev/vda)" = /dev/vda1 ]
		[ "$(partition_path /dev/nbd0)" = /dev/nbd0p1 ]
	' sh "$repo/adapters/common.sh"

for tool in blkid sfdisk; do
	qemu-aarch64 "$root/sbin/$tool" --version
done
echo 'ARM64 initramfs shell, adapters, DHCP hook and disk tools passed'
