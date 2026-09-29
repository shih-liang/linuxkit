#!/bin/sh
# Shared by recovery installation and the installed boot service.
set -eu
pages=$(awk '/^KernelPageSize:/ { print $2; exit }' /proc/self/smaps)
if [ "$pages" != 4 ]; then
	echo "Rosetta requires a 4 KiB ARM64 kernel; this kernel uses ${pages:-unknown} KiB pages. Select the FluxWindow Rosetta kernel." >&2
	exit 1
fi
mkdir -p /run/rosetta /proc/sys/fs/binfmt_misc
mountpoint -q /run/rosetta || mount -t virtiofs rosetta /run/rosetta
if [ ! -x /run/rosetta/rosetta ]; then
	echo 'Rosetta is unavailable. Enable Rosetta for this VM in FluxWindow.' >&2
	exit 1
fi
if [ ! -e /proc/sys/fs/binfmt_misc/register ]; then
	mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
fi
if [ -e /proc/sys/fs/binfmt_misc/rosetta ]; then
	for expected in 'interpreter /run/rosetta/rosetta' enabled 'flags: POCF' \
		'magic 7f454c4602010100000000000000000002003e00' \
		'mask fffffffffffefe00fffffffffffffffffeffffff'; do
		if ! grep -Fxq "$expected" /proc/sys/fs/binfmt_misc/rosetta; then
			echo 'An incompatible Rosetta binfmt handler is already registered. Remove the conflicting registration before enabling FluxWindow Rosetta.' >&2
			exit 1
		fi
	done
else
	# Escaped NULs must reach binfmt_misc literally: printf %b corrupts this
	# match and can accidentally capture native ARM executables.
	printf '%s\n' ':rosetta:M::\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x3e\x00:\xff\xff\xff\xff\xff\xfe\xfe\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/run/rosetta/rosetta:POCF' \
		> /proc/sys/fs/binfmt_misc/register
fi
