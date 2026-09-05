#!/bin/sh
# Run the actual ARM64 recovery tools, not the runner's full-featured /bin/sh.
set -eu

root=${1:?assembled initramfs directory is required}
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
busybox="$root/bin/busybox"
test -x "$busybox"
for tool in /usr/sbin/partprobe /usr/sbin/chroot /sbin/mkfs.ext4 \
	/sbin/e2fsck /sbin/resize2fs /sbin/udhcpc /sbin/ip; do
	test -x "$root$tool"
done

run_busybox()
{
	run_tool "$busybox" "$@"
}

run_tool()
{
	if [ "$(uname -m)" = aarch64 ]; then
		"$@"
	else
		qemu-aarch64 "$@"
	fi
}

for adapter in "$repo"/adapters/*.sh; do
	run_busybox sh -n "$adapter"
done

# These are used by the partition-device wait loops and the DHCP hook.
run_busybox sh -ec 'test "$((2147483647 + 1))" = 2147483648'
run_busybox sleep 0.01
run_busybox sh "$repo/adapters/udhcpc.sh" --self-test

for tool in blkid sfdisk; do
	run_tool "$root/sbin/$tool" --version
done
echo 'ARM64 initramfs shell, adapters, DHCP hook and disk tools passed'
