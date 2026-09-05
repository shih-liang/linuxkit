#!/bin/sh
set -eu
. "${NP_SOURCE_PATH%/*}/common.sh"

configure_amd64_wine()
{
	selected wine || return 0
	. /etc/os-release
	: "${VERSION_CODENAME:?Ubuntu rootfs has no VERSION_CODENAME}"
	dpkg --add-architecture amd64

	# Ubuntu's ARM image points at ports.ubuntu.com, which does not publish
	# amd64 packages. Keep native packages on ubuntu-ports and obtain only the
	# foreign architecture from the regular Ubuntu archive.
	cat > /etc/apt/sources.list.d/ubuntu.sources <<EOF
Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports/
Suites: $VERSION_CODENAME $VERSION_CODENAME-updates $VERSION_CODENAME-backports
Components: main universe restricted multiverse
Architectures: arm64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports/
Suites: $VERSION_CODENAME-security
Components: main universe restricted multiverse
Architectures: arm64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

Types: deb
URIs: http://archive.ubuntu.com/ubuntu/
Suites: $VERSION_CODENAME $VERSION_CODENAME-updates $VERSION_CODENAME-backports
Components: main universe restricted multiverse
Architectures: amd64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

Types: deb
URIs: http://security.ubuntu.com/ubuntu/
Suites: $VERSION_CODENAME-security
Components: main universe restricted multiverse
Architectures: amd64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF
}

install_steam()
{
	selected steam || return 0
	add-apt-repository -y ppa:fex-emu/fex
	apt-get update
	# Steam is launched explicitly with FEXBash below. Installing FEX's global
	# binfmt packages would steal x86-64 Wine processes from Apple Rosetta.
	apt-get install -y --no-install-recommends fex-emu-armv8.0
	runuser -u nativepipe -- env HOME=/home/nativepipe \
		XDG_DATA_HOME=/home/nativepipe/.local/share \
		FEXRootFSFetcher -y -a --distro-name Ubuntu \
		--distro-version 24.04 --distro-list-first
	steam_deb=/var/tmp/steam-launcher.deb
	curl --fail --location --retry 5 \
		https://repo.steampowered.com/steam/archive/stable/steam-launcher_latest_all.deb \
		-o "$steam_deb"
	dpkg-deb --extract "$steam_deb" /
	rm -f "$steam_deb" /usr/share/applications/steam.desktop
	mkdir -p /usr/local/bin /usr/share/applications
	cat > /usr/local/bin/lighthouse-steam <<'EOF'
#!/bin/sh
export STEAMOS=1 STEAM_RUNTIME=1
exec FEXBash -c steam "$@"
EOF
	chmod 0755 /usr/local/bin/lighthouse-steam
	cat > /usr/share/applications/lighthouse-steam.desktop <<'EOF'
[Desktop Entry]
Name=Steam
Comment=Steam through FEX x86 emulation
Exec=/usr/local/bin/lighthouse-steam %U
Icon=steam
Terminal=false
Type=Application
Categories=Game;
MimeType=x-scheme-handler/steam;
EOF
}

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
		iproute2 util-linux kmod passwd sudo ca-certificates pipewire-audio \
		xdg-user-dirs gsettings-desktop-schemas fonts-dejavu-core adwaita-icon-theme \
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
	configure_amd64_wine
	packages=
	selected developer-tools && packages="$packages build-essential curl git"
	# Install the x86-64 Wine process and its amd64 libraries. Rosetta handles
	# the ELF loader; --no-install-recommends deliberately excludes wine32/i386.
	selected wine && packages="$packages wine wine64:amd64"
	selected steam && packages="$packages curl software-properties-common"
	export DEBIAN_FRONTEND=noninteractive
	if [ -n "$packages" ]; then
		apt-get update
		apt-get install -y --no-install-recommends $packages
	fi
	install_steam
	;;
repair)
	grow_root_disk
	;;
*) fail "ubuntu adapter expects install, repair or software" ;;
esac
