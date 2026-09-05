#!/bin/sh
set -eu
. "${NP_SOURCE_PATH%/*}/common.sh"

case ${1:-} in
install)
	prepare_root_disk
	(
	work=$(mktemp -d /tmp/fedora-oci.XXXXXX)
	trap 'rm -rf "$work"' EXIT
	tar -xpf "$NP_SOURCE_PATH" -C "$work"
	layers=0
	for blob in "$work"/blobs/sha256/*; do
		if tar -tf "$blob" >/dev/null 2>&1; then
			layer=$blob
			layers=$((layers + 1))
		fi
	done
	[ "$layers" -eq 1 ] || fail "Fedora OCI archive did not contain one rootfs layer"
	tar -xpf "$layer" -C "$NP_TARGET_ROOT"
	)
	ensure_install_network
	run_in_target "$NP_TARGET_ROOT" /usr/bin/dnf -y install \
		systemd systemd-pam systemd-udev systemd-networkd systemd-resolved \
		util-linux-core shadow-utils iproute dbus-daemon sudo ca-certificates \
		xdg-user-dirs gsettings-desktop-schemas dejavu-sans-fonts adwaita-icon-theme \
		kmod pipewire pipewire-alsa pipewire-pulseaudio wireplumber \
		mesa-dri-drivers mesa-libEGL mesa-libGL mesa-vulkan-drivers \
		libglvnd-gles libwayland-client libwayland-server libxkbcommon libxcb \
		xcb-util-cursor vulkan-loader xorg-x11-server-Xwayland \
		xwayland-satellite
	finish_rootfs
	;;
software)
	if selected developer-tools; then
		dnf -y install @development-tools curl git
	fi
	;;
repair)
	grow_root_disk
	;;
*) fail "fedora adapter expects install, repair or software" ;;
esac
