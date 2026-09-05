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
	qemu-aarch64 "$busybox" "$@"
}

for adapter in "$repo"/adapters/*.sh; do
	run_busybox sh -n "$adapter"
done

# These are used by the partition-device wait loops and the DHCP hook.
run_busybox sh -ec 'test "$((2147483647 + 1))" = 2147483648; sleep 0.01'
run_busybox sleep 0.01
run_busybox sh "$repo/adapters/udhcpc.sh" --self-test

# Execute deconfiguration with an ip stub: DHCPv4 must not flush the IPv6
# link-local address which networkd needs after switch_root.
run_busybox sh -ec '
	ip() {
		case "$*" in
		"link set dev test0 up"|"-4 addr flush dev test0") return 0 ;;
		*) echo "unexpected DHCP command: ip $*" >&2; return 1 ;;
		esac
	}
	hook=$1
	interface=test0
	set -- deconfig
	. "$hook"
' sh "$repo/adapters/udhcpc.sh"

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
