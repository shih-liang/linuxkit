#!/bin/sh
# Shared installer primitives. Distribution adapters are the policy; this file
# only supplies the disk/rootfs operations that are identical for every distro.
set -eu

PAYLOAD_ROOT=${NP_SOURCE_PATH%/*}
SELECTION_FILE="$PAYLOAD_ROOT/selection"

fail()
{
	echo "lighthouse installer: $*" >&2
	exit 1
}

unmount_target()
(
	# Daemon shutdown may return before the kernel releases its last file
	# references. Wait for actual unmount, never detach a still-busy mount.
	remaining=100
	while ! umount "$1" 2>/dev/null; do
		remaining=$((remaining - 1))
		[ "$remaining" -gt 0 ] || { umount "$1"; return $?; }
		sleep 0.01
	done
)

cleanup_target_root()
{
	[ "${NP_TARGET_ROOT:-/}" != / ] || return 0
	unmount_target_runtime "$NP_TARGET_ROOT" || return 1
	! mountpoint -q "$NP_TARGET_ROOT" || unmount_target "$NP_TARGET_ROOT"
}

trap 'status=$?; trap - EXIT; cleanup_target_root || status=1; exit "$status"' EXIT
trap 'exit 1' HUP INT TERM

selected()
{
	[ -f "$SELECTION_FILE" ] && grep -Fxq "$1" "$SELECTION_FILE"
}

ensure_install_network()
{
	# Recovery may already have a lease from a previous/manual attempt.
	if [ -s /etc/resolv.conf ] && ip -4 route show default | grep -q '^default'; then
		return 0
	fi
	for interface_path in /sys/class/net/*; do
		interface_name=${interface_path##*/}
		# Built-in bonding/tunnel drivers also create interfaces, but they
		# have no backing device and cannot obtain an install-time DHCP lease.
		[ -e "$interface_path/device" ] || continue
		ip link set dev "$interface_name" up || continue
		if udhcpc -q -n -t 5 -T 3 -i "$interface_name"; then
			[ -s /etc/resolv.conf ] || fail "DHCP returned no DNS servers"
			return 0
		fi
	done
	fail "no network interface obtained a DHCP lease"
}

mount_target_runtime()
{
	root=$1
	# Bind only the installer payload, not all of initramfs /run. Package
	# installation and optional software finish before switch_root/guestd.
	for path in dev dev/pts proc sys run/nativepipe/payload; do
		mkdir -p "$root/$path" || return 1
		mount -o bind "/$path" "$root/$path" || return 1
	done
	mkdir -p "$root/etc" || return 1
	rm -f "$root/etc/resolv.conf" && cp /etc/resolv.conf "$root/etc/resolv.conf"
}

unmount_target_runtime()
{
	root=$1 np_unmount_result=0
	for path in run/nativepipe/payload sys proc dev/pts dev; do
		if mountpoint -q "$root/$path"; then
			unmount_target "$root/$path" || np_unmount_result=1
		fi
	done
	return "$np_unmount_result"
}

run_in_target()
(
	root=$1
	shift
	# A signal or any failed mount must take the same cleanup path as chroot.
	trap 'status=$?; trap - EXIT; unmount_target_runtime "$root" || status=1; exit "$status"' EXIT
	trap 'exit 1' HUP INT TERM
	mount_target_runtime "$root" || return 1
	chroot "$root" "$@"
)

partition_path()
{
	disk=$1
	case "$disk" in
	*[0-9]) printf '%sp1\n' "$disk" ;;
	*) printf '%s1\n' "$disk" ;;
	esac
}

prepare_root_disk()
{
	: "${NP_TARGET_DISK:?missing NP_TARGET_DISK}"
	: "${NP_TARGET_ROOT:?missing NP_TARGET_ROOT}"
	cleanup_target_root || fail "target is still mounted; refusing to format it"

	/sbin/sfdisk --wipe always "$NP_TARGET_DISK" <<'EOF'
label: gpt
unit: sectors

start=2048, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name="nativepipe-root"
EOF
	partition=$(partition_path "$NP_TARGET_DISK")
	i=0
	while [ ! -b "$partition" ]; do
		[ "$i" -lt 100 ] || fail "partition device did not appear: $partition"
		i=$((i + 1))
		sleep 0.1
	done
	/sbin/mkfs.ext4 -F -L nativepipe-root "$partition"
	mount -t ext4 -o rw "$partition" "$NP_TARGET_ROOT"
}

grow_root_disk()
{
	: "${NP_TARGET_DISK:?missing NP_TARGET_DISK}"
	partition=$(partition_path "$NP_TARGET_DISK")
	[ -b "$partition" ] || fail "root partition does not exist: $partition"
	cleanup_target_root || fail "target is still mounted; refusing offline filesystem repair"

	# The adapters in this catalog created one GPT ext4 root partition.  The
	# host has already enlarged only the image; this adapter-owned operation
	# extends partition 1 to the final sector without recreating the table.
	/sbin/sfdisk --force -N 1 "$NP_TARGET_DISK" <<'EOF'
size=+
EOF

	set +e
	/sbin/e2fsck -pf "$partition"
	fsck_status=$?
	set -e
	case $fsck_status in
	0|1) ;;
	*) fail "e2fsck failed with status $fsck_status" ;;
	esac
	/sbin/resize2fs "$partition"
	sync
}

systemd_unit_directory()
{
	root=$1
	for directory in /usr/lib/systemd/system /lib/systemd/system; do
		[ -d "$root$directory" ] && { printf '%s\n' "$directory"; return 0; }
	done
	return 1
}

enable_systemd_unit()
{
	root=$1 target=$2 unit=$3 source_unit=${4:-$3}
	unit_directory=$(systemd_unit_directory "$root") ||
		fail "rootfs has no systemd unit directory"
	mkdir -p "$root/etc/systemd/system/$target.wants"
	if [ -f "$root/etc/systemd/system/$source_unit" ]; then
		source_path=/etc/systemd/system/$source_unit
	elif [ -f "$root$unit_directory/$source_unit" ]; then
		source_path=$unit_directory/$source_unit
	else
		fail "rootfs has no systemd unit $source_unit"
	fi
	ln -sf "$source_path" "$root/etc/systemd/system/$target.wants/$unit"
}

configure_network()
{
	root=$1
	mkdir -p "$root/etc/systemd/network"
	cat > "$root/etc/systemd/network/20-lighthouse.network" <<'EOF'
[Match]
Name=en* eth*

[Network]
DHCP=yes
IPv6AcceptRA=yes
EOF
	rm -f "$root/etc/resolv.conf"
	ln -s /run/systemd/resolve/stub-resolv.conf "$root/etc/resolv.conf"
	enable_systemd_unit "$root" multi-user.target systemd-networkd.service
	enable_systemd_unit "$root" multi-user.target systemd-resolved.service
	enable_systemd_unit "$root" network-online.target systemd-networkd-wait-online.service
}

create_default_user()
{
	root=$1 user=${2:-nativepipe}
	if ! grep -q "^${user}:" "$root/etc/passwd"; then
		useradd=/usr/sbin/useradd
		[ -x "$root$useradd" ] || useradd=/sbin/useradd
		[ -x "$root$useradd" ] || fail "rootfs has no useradd"
		run_in_target "$root" "$useradd" -m -s /bin/bash "$user"
	fi

	groups=
	for group in sudo wheel audio video render input; do
		if grep -q "^${group}:" "$root/etc/group"; then
			groups=${groups:+$groups,}$group
		fi
	done
	if [ -n "$groups" ]; then
		usermod=/usr/sbin/usermod
		[ -x "$root$usermod" ] || usermod=/sbin/usermod
		[ -x "$root$usermod" ] && run_in_target "$root" "$usermod" -aG "$groups" "$user"
	fi

	mkdir -p "$root/etc/systemd/system/serial-getty@hvc0.service.d"
	cat > "$root/etc/systemd/system/serial-getty@hvc0.service.d/autologin.conf" <<EOF
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin $user --noclear %I \$TERM
EOF
	enable_systemd_unit \
		"$root" getty.target serial-getty@hvc0.service serial-getty@.service
}

install_guest_agent()
{
	root=$1
	agent="$PAYLOAD_ROOT/agent"
	[ -x "$agent/nativepipe-guestd" ] || fail "payload has no nativepipe-guestd"
	[ -f "$agent/systemd/nativepipe-guestd.service" ] ||
		fail "payload has no guestd systemd unit"
	mkdir -p "$root/usr/libexec/nativepipe" "$root/etc/systemd/system"
	cp "$agent/nativepipe-guestd" "$root/usr/libexec/nativepipe/nativepipe-guestd"
	chmod 0755 "$root/usr/libexec/nativepipe/nativepipe-guestd"
	cp "$agent/systemd/nativepipe-guestd.service" \
		"$root/etc/systemd/system/nativepipe-guestd.service"
	enable_systemd_unit "$root" multi-user.target nativepipe-guestd.service
}

install_rosetta_support()
{
	root=$1
	# Wine is an amd64 Linux process and therefore needs the same translation
	# service as a directly launched x86-64 program. Steam invokes FEXBash
	# explicitly, so it can coexist without installing a competing binfmt entry.
	selected x86_64 || selected wine || return 0
	mkdir -p "$root/usr/libexec/nativepipe" "$root/etc/systemd/system"
	cat > "$root/usr/libexec/nativepipe/mount-rosetta" <<'EOF'
#!/bin/sh
set -eu
mkdir -p /run/rosetta /proc/sys/fs/binfmt_misc
mountpoint -q /run/rosetta || mount -t virtiofs rosetta /run/rosetta
[ -x /run/rosetta/rosetta ] || exit 1
if [ ! -e /proc/sys/fs/binfmt_misc/register ]; then
	mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
fi
[ -e /proc/sys/fs/binfmt_misc/rosetta ] ||
	printf '%s\n' ':rosetta:M::\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x3e\x00:\xff\xff\xff\xff\xff\xfe\xfe\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/run/rosetta/rosetta:POCF' \
		> /proc/sys/fs/binfmt_misc/register
EOF
	chmod 0755 "$root/usr/libexec/nativepipe/mount-rosetta"
	cat > "$root/etc/systemd/system/lighthouse-rosetta.service" <<'EOF'
[Unit]
Description=Mount Apple Rosetta for Linux
Before=nativepipe-guestd.service

[Service]
Type=oneshot
ExecStart=/usr/libexec/nativepipe/mount-rosetta
RemainAfterExit=yes
EOF
	enable_systemd_unit "$root" multi-user.target lighthouse-rosetta.service
}

install_selected_software()
{
	root=$1
	[ -s "$SELECTION_FILE" ] || return 0
	run_in_target "$root" /usr/bin/env \
		NP_TARGET_ROOT=/ NP_SOURCE_PATH=/run/nativepipe/payload/source \
		/bin/sh /run/nativepipe/payload/adapter.sh software
}

finish_rootfs()
{
	root=${NP_TARGET_ROOT:?missing NP_TARGET_ROOT}
	[ -x "$root/sbin/init" ] || fail "installed rootfs has no /sbin/init"
	printf 'nativepipe\n' > "$root/etc/hostname"
	printf 'LABEL=nativepipe-root / ext4 defaults 0 1\n' > "$root/etc/fstab"
	: > "$root/etc/machine-id"
	create_default_user "$root" nativepipe
	install_selected_software "$root"
	# Chroot package operations use install-time DNS; publish the distro's
	# normal boot resolver configuration only after the last chroot exits.
	configure_network "$root"
	install_guest_agent "$root"
	install_rosetta_support "$root"
	sync
}
