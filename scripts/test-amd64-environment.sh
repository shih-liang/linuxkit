#!/bin/sh
# Run as root inside an explicitly selected disposable, installed test VM.
set -eu
user=${1:?usage: test-amd64-environment.sh TEST_VM_USERNAME}
uid=$(id -u "$user")
[ "$uid" -ge 1000 ] && [ "$uid" -lt 65534 ]
[ "$(id -u)" = 0 ]
[ "$(dpkg --print-architecture)" = arm64 ]
[ "$(getconf PAGESIZE)" = 4096 ]
[ -x /usr/local/bin/nativepipe-amd64 ]
home=$(getent passwd "$user" | cut -d: -f6)
work=$(mktemp -d "$home/.nativepipe-amd64-test.XXXXXX")
chown "$uid" "$work"
cleanup() {
	if mountpoint -q "$work/nested"; then umount "$work/nested" || return 1; fi
	rm -rf "$work"
}
trap cleanup EXIT HUP INT TERM
run() { runuser -u "$user" -- /usr/local/bin/nativepipe-amd64 "$@"; }
sessions() { schroot --list --all-sessions | grep nativepipe-amd64 || :; }
before=$(sessions)
[ "$(run dpkg --print-architecture)" = amd64 ]
[ "$(run id -u)" = "$uid" ]
[ "$(run /bin/sh -c 'printf "%s" "$1"' sh 'literal ; $HOME "two words"')" = 'literal ; $HOME "two words"' ]
if run /bin/sh -c 'exit 7'; then exit 1; else [ "$?" = 7 ]; fi
run /bin/sh -c 'printf shared-home > "$1"' sh "$work/from-amd64"
[ "$(cat "$work/from-amd64")" = shared-home ]
mkdir "$work/nested"
mount -t tmpfs -o size=1m,mode=0755 tmpfs "$work/nested"
printf nested-share > "$work/nested/marker"
[ "$(run cat "$work/nested/marker")" = nested-share ]
# /run/user/UID is a separate systemd tmpfs. A plain bind of /run/user
# silently exposes an empty directory instead of the compositor's sockets.
if [ -d "/run/user/$uid" ]; then
	[ "$(run stat -c '%d:%i' "/run/user/$uid")" = "$(stat -c '%d:%i' "/run/user/$uid")" ]
fi
if timeout 5 runuser -u "$user" -- schroot -c nativepipe-amd64 -u root -- id </dev/null; then
	echo 'Unexpected passwordless root access' >&2; exit 1
else
	status=$?
	[ "$status" != 124 ] && [ "$status" != 137 ]
fi
pids=
for index in 1 2 3 4; do
	run /bin/sh -c 'sleep 0.1; test "$(dpkg --print-architecture)" = amd64' &
	pids="$pids $!"
done
for pid in $pids; do wait "$pid"; done
if timeout 1 runuser -u "$user" -- nativepipe-amd64 /bin/sleep 60; then
	echo 'Cancellation failed' >&2; exit 1
else
	[ "$?" = 124 ]
fi
[ "$(sessions)" = "$before" ]
[ "$(cat "$work/nested/marker")" = nested-share ]
if findmnt -rn -o TARGET | grep '/var/lib/schroot/mount/nativepipe-amd64-'; then
	echo 'Session mounts leaked' >&2; exit 1
fi
systemctl is-active nativepipe-guestd.service lighthouse-rosetta.service
printf 'AMD64_ENVIRONMENT_OK: architecture, UID, argv, exit, nested shares, user runtime, privilege, concurrent sessions, cancellation and cleanup\n'
