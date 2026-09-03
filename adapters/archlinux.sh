#!/bin/sh
set -eu
. /run/nativepipe/payload/common.sh

case ${1:-} in
install)
	prepare_root_disk
	tar -xpf "$NP_SOURCE_PATH" -C "$NP_TARGET_ROOT"
	if [ ! -x "$NP_TARGET_ROOT/sbin/init" ]; then
		ensure_install_network
		run_in_target "$NP_TARGET_ROOT" /usr/bin/pacman -Syu --noconfirm \
			--needed systemd iproute2 util-linux kmod shadow sudo \
			ca-certificates
	fi
	ensure_install_network
	run_in_target "$NP_TARGET_ROOT" /usr/bin/pacman -Syu --noconfirm \
		--needed kmod pipewire pipewire-audio pipewire-pulse wireplumber \
		mesa vulkan-virtio wayland libxkbcommon libxcb xcb-util-cursor \
		vulkan-icd-loader xorg-xwayland
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
