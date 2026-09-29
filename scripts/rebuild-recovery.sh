#!/bin/sh
# Reuse checksum-verified recovery tools when iterating on the C programs.
# Release CI builds those tools from their pinned sources instead.
set -eu
[ "$#" = 5 ] || { echo 'usage: rebuild-recovery BASE SHA256 INIT INSTALLER OUTPUT' >&2; exit 2; }
base=$1 expected=$2 init=$3 installer=$4 output=$5
printf '%s  %s\n' "$expected" "$base" | sha256sum -c -
for program in "$init" "$installer"; do
	[ -x "$program" ]
	if readelf -l "$program" | grep -q INTERP; then
		echo "Recovery requires a static executable: $program" >&2; exit 1
	fi
done
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT HUP INT TERM
gzip -dc "$base" > "$stage/base.cpio"
mkdir "$stage/root"
(cd "$stage/root" && cpio -idmu --quiet < ../base.cpio)
install -m0755 "$init" "$stage/root/sbin/nativepipe-init"
install -m0755 "$installer" "$stage/root/sbin/nativepipe-install"
strip "$stage/root/sbin/nativepipe-init" "$stage/root/sbin/nativepipe-install"
find "$stage/root" -exec touch -h -d '@0' {} +
(cd "$stage/root" && find . -print0 | LC_ALL=C sort -z | cpio -o -0 -H newc --quiet) > "$stage/result.cpio"
gzip -n -9 -c "$stage/result.cpio" > "$output"
sha256sum "$output"
