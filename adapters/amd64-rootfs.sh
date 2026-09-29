#!/bin/sh
# Run inside the native Ubuntu installation. schroot owns runtime sessions,
# authorization, session mounts and recovery; no custom setuid code.
set -eu
. "${NP_SOURCE_PATH%/*}/common.sh"
selected amd64-rootfs || exit 0
[ "$(dpkg --print-architecture)" = arm64 ] || fail 'the base system must remain ARM64'
[ -s "$PAYLOAD_ROOT/source-amd64" ] || fail 'verified amd64 rootfs is missing'
[ -x /run/rosetta/rosetta ] || fail 'Rosetta was not prepared before installation'
destination=/var/lib/nativepipe/amd64
[ ! -e "$destination" ] || fail 'amd64 environment already exists; refusing to replace its files'
mkdir -p /var/lib/nativepipe
stage=$(mktemp -d /var/lib/nativepipe/.amd64-install.XXXXXX)
# A failed stage is retained for diagnostics. run_in_target unwinds all mounts;
# never recursively remove a directory when a mount could still be busy.
tar -xpf "$PAYLOAD_ROOT/source-amd64" -C "$stage"
chmod 0755 "$stage"
mkdir -p "$stage/usr/sbin"
printf '#!/bin/sh\nexit 101\n' > "$stage/usr/sbin/policy-rc.d"
chmod 0755 "$stage/usr/sbin/policy-rc.d"
run_in_target "$stage" /bin/true || fail 'Rosetta cannot execute the selected amd64 rootfs'
[ "$(run_in_target "$stage" /usr/bin/dpkg --print-architecture)" = amd64 ] ||
	fail 'the additional rootfs must be amd64'
run_in_target "$stage" /usr/bin/env DEBIAN_FRONTEND=noninteractive /bin/sh -ec '
	apt-get update
	apt-get install -y --no-install-recommends bash ca-certificates curl git sudo passwd \
		locales dbus libwayland-client0 libxkbcommon0 libgl1-mesa-dri libegl-mesa0 \
		libgl1 libegl1 libgles2 libvulkan1 mesa-vulkan-drivers fonts-dejavu-core
	apt-get clean
'
# Match the existing account and sudo policy, including its password hash.
# Cleartext installation credentials never enter this filesystem.
for name in passwd group shadow gshadow; do cp -p "/etc/$name" "$stage/etc/$name"; done
mkdir -p "$stage/etc/sudoers.d"
cp -p /etc/sudoers.d/90-fluxwindow-user "$stage/etc/sudoers.d/90-fluxwindow-user"
# Reuse NativePipe's existing 4 KiB guest compatibility layer, built for amd64.
# schroot deliberately drops the ARM loader's LD_PRELOAD; the entry below
# reapplies the matching libraries only after entering the amd64 filesystem.
mkdir -p "$stage/usr/libexec/nativepipe" "$stage/etc/vulkan/implicit_layer.d" "$stage/etc/profile.d"
install -m 0755 "$PAYLOAD_ROOT/amd64-graphics/nativepipe-align-blob-x86_64-gnu.so" \
	"$stage/usr/libexec/nativepipe/nativepipe-align-host-blob.so"
install -m 0755 "$PAYLOAD_ROOT/amd64-graphics/nativepipe-vulkan-layer-x86_64-gnu.so" \
	"$stage/usr/libexec/nativepipe/nativepipe-vulkan-blob-alignment.so"
install -m 0644 "$PAYLOAD_ROOT/amd64-graphics/VkLayer_NATIVEPIPE_blob_alignment.json" \
	"$stage/etc/vulkan/implicit_layer.d/"
install -m 0644 "$PAYLOAD_ROOT/amd64-graphics/nativepipe.sh" "$stage/etc/profile.d/"
cat > "$stage/usr/libexec/nativepipe/amd64-session" <<'EOF'
#!/bin/sh
set -eu
. /etc/profile.d/nativepipe.sh
if [ "$#" = 0 ]; then exec "${SHELL:-/bin/bash}" -l; fi
exec "$@"
EOF
chmod 0755 "$stage/usr/libexec/nativepipe/amd64-session"
# Package upgrades must not start another init or control the native services.
printf 'This amd64 environment shares the ARM64 VM kernel. Enter with nativepipe-amd64.\n' > "$stage/etc/motd"
mv "$stage" "$destination"

IFS= read -r install_user < "$PAYLOAD_ROOT/account"
mkdir -p /etc/schroot/chroot.d /etc/schroot/nativepipe-amd64 /run/user /mnt/lighthouse
cat > /etc/schroot/chroot.d/nativepipe-amd64.conf <<EOF
[nativepipe-amd64]
description=Ubuntu amd64 through Rosetta
type=directory
directory=$destination
users=$install_user
profile=nativepipe-amd64
preserve-environment=true
EOF
# Only session sockets are shared from /run. Sharing all of /run would expose
# the native system manager to package maintainer scripts in the amd64 root.
# Recursive slave mounts include systemd's per-user tmpfs and shared-directory
# submounts without propagating changes back to the native system.
cat > /etc/schroot/nativepipe-amd64/fstab <<'EOF'
/proc /proc none bind 0 0
/sys /sys none bind 0 0
/dev /dev none bind 0 0
/dev/pts /dev/pts none bind 0 0
/dev/shm /dev/shm none bind 0 0
/home /home none rbind,rslave 0 0
/tmp /tmp none bind 0 0
/run/user /run/user none rbind,rslave 0 0
/run/rosetta /run/rosetta none bind 0 0
/mnt/lighthouse /mnt/lighthouse none rbind,rslave 0 0
EOF
printf '/etc/resolv.conf\n/etc/hosts\n' > /etc/schroot/nativepipe-amd64/copyfiles
printf 'passwd\nshadow\ngroup\ngshadow\n' > /etc/schroot/nativepipe-amd64/nssdatabases
mkdir -p /usr/local/bin
cat > /usr/local/bin/nativepipe-amd64 <<'EOF'
#!/bin/sh
set -eu
if [ ! -r /proc/sys/fs/binfmt_misc/rosetta ] || [ ! -x /run/rosetta/rosetta ]; then
	echo 'The amd64 environment needs Rosetta. Enable it in FluxWindow and restart this VM.' >&2
	exit 1
fi
# schroot preserves arguments, exit status, terminal control and the invoking
# UID. Its standard environment filter removes loader-injection variables.
exec /usr/bin/schroot --quiet --chroot=nativepipe-amd64 --preserve-environment \
	-- /usr/libexec/nativepipe/amd64-session "$@"
EOF
chmod 0755 /usr/local/bin/nativepipe-amd64
