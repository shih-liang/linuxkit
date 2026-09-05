#!/bin/sh
set -eu
. /run/nativepipe/payload/common.sh

case ${1:-} in
install)
	prepare_root_disk
	tar -xpf "$NP_SOURCE_PATH" -C "$NP_TARGET_ROOT"
	ensure_install_network
	# The official ARM rootfs requires keyring initialization before pacman.
	# Keep its GPG agent inside this operation so it cannot pin the chroot mounts.
	run_in_target "$NP_TARGET_ROOT" /bin/sh -ec '
		trap "gpgconf --homedir /etc/pacman.d/gnupg --kill all" EXIT
		pacman-key --init
		pacman-key --populate archlinuxarm
		pacman -Syu --noconfirm --needed \
			systemd iproute2 util-linux kmod shadow sudo ca-certificates \
			pipewire pipewire-audio pipewire-pulse wireplumber \
			mesa vulkan-virtio wayland libxkbcommon libxcb xcb-util-cursor \
			vulkan-icd-loader xorg-xwayland xwayland-satellite
	'
	finish_rootfs
	;;
software)
	enable_rosetta
	packages="ca-certificates dbus sudo xdg-user-dirs"
	selected developer-tools && packages="$packages base-devel curl git"
	pacman -Syu --noconfirm --needed $packages
	;;
repair)
	grow_root_disk
	;;
*) fail "archlinux adapter expects install, repair or software" ;;
esac
