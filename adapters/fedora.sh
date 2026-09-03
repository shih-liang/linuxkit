#!/bin/sh
set -eu
. /run/nativepipe/payload/common.sh

case ${1:-} in
install)
	prepare_root_disk
	work=/tmp/fedora-oci
	rm -rf "$work"
	mkdir -p "$work"
	tar -xpf "$NP_SOURCE_PATH" -C "$work"
	layers=0
	for blob in "$work"/blobs/sha256/*; do
		if tar -tf "$blob" >/dev/null 2>&1; then
			tar -xpf "$blob" -C "$NP_TARGET_ROOT"
			layers=$((layers + 1))
		fi
	done
	[ "$layers" -eq 1 ] || fail "Fedora OCI archive did not contain one rootfs layer"
	rm -rf "$work"
	ensure_install_network
	run_in_target "$NP_TARGET_ROOT" /usr/bin/dnf -y install \
		systemd systemd-udev systemd-networkd systemd-resolved \
		util-linux-core shadow-utils iproute dbus-daemon sudo ca-certificates \
		kmod pipewire pipewire-alsa pipewire-pulseaudio wireplumber \
		mesa-dri-drivers mesa-libEGL mesa-libGL mesa-vulkan-drivers \
		libglvnd-gles wayland-libs libxkbcommon libxcb \
		xcb-util-cursor vulkan-loader xorg-x11-server-Xwayland
	finish_rootfs
	;;
software)
	enable_rosetta
	packages="ca-certificates dbus-daemon sudo xdg-user-dirs"
	selected developer-tools && packages="$packages @development-tools curl git"
	dnf -y install $packages
	;;
repair)
	grow_root_disk
	;;
*) fail "fedora adapter expects install, repair or software" ;;
esac
