#!/bin/sh
set -eu
. "${NP_SOURCE_PATH%/*}/common.sh"

case ${1:-} in
install)
	prepare_root_disk
	tar -xpf "$NP_SOURCE_PATH" -C "$NP_TARGET_ROOT"
	ensure_install_network
	mkdir -p "$NP_TARGET_ROOT/usr/sbin"
	cat > "$NP_TARGET_ROOT/usr/sbin/policy-rc.d" <<'EOF'
#!/bin/sh
exit 101
EOF
	chmod 0755 "$NP_TARGET_ROOT/usr/sbin/policy-rc.d"
	export DEBIAN_FRONTEND=noninteractive
	run_in_target "$NP_TARGET_ROOT" /usr/bin/apt-get update
	run_in_target "$NP_TARGET_ROOT" /usr/bin/apt-get install -y \
		--no-install-recommends systemd-sysv systemd-resolved udev dbus-user-session \
		iproute2 util-linux kmod passwd sudo ca-certificates pipewire-audio rtkit \
		libpam-systemd libpam-modules libpam-runtime \
		xdg-user-dirs gsettings-desktop-schemas fonts-dejavu-core adwaita-icon-theme \
		libglib2.0-0t64 libgdk-pixbuf-2.0-0 librsvg2-2 \
		libgl1 libgles2 libegl1 libgl1-mesa-dri libegl-mesa0 libglx-mesa0 \
		mesa-vulkan-drivers libwayland-server0 libxkbcommon0 \
		libxcb1 libxcb-cursor0 xwayland
	# Ubuntu currently does not publish xwayland-satellite, but probing the
	# package index keeps this adapter correct when it becomes available. X11
	# integration remains disabled until the distribution can install it;
	# NativePipe does not download or execute a private replacement.
	if run_in_target "$NP_TARGET_ROOT" /usr/bin/apt-cache show \
		xwayland-satellite >/dev/null 2>&1; then
		run_in_target "$NP_TARGET_ROOT" /usr/bin/apt-get install -y \
			--no-install-recommends xwayland-satellite
	else
		echo "lighthouse installer: Ubuntu repository has no xwayland-satellite; X11 integration is unavailable" >&2
	fi
	finish_rootfs
	rm -f "$NP_TARGET_ROOT/usr/sbin/policy-rc.d"
	;;
software)
	packages=
	selected amd64-rootfs && packages="$packages schroot"
	selected developer-tools && packages="$packages build-essential curl git"
	export DEBIAN_FRONTEND=noninteractive
	if [ -n "$packages" ]; then
		apt-get update
		apt-get install -y --no-install-recommends $packages
	fi
	if selected amd64-rootfs; then /bin/sh "$PAYLOAD_ROOT/amd64-rootfs.sh"; fi
	;;
repair)
	grow_root_disk
	;;
*) fail "ubuntu adapter expects install, repair or software" ;;
esac
