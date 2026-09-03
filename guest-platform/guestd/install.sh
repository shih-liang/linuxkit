#!/bin/sh
#
# Installs the NativePipe guest agent into the running rootfs.
# Runs the static guestd binary with --provision.
#
#   sh /path/to/guest/guestd/install.sh

set -eu

SOURCE_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ "$(id -u)" != 0 ]; then
	echo "install.sh must run as root" >&2
	exit 1
fi

ARCH="$(uname -m)"
case "${ARCH}" in
aarch64|arm64) BIN_NAME=nativepipe-guestd-aarch64 ;;
x86_64) BIN_NAME=nativepipe-guestd-x86_64 ;;
*)
	echo "error: unsupported arch ${ARCH}" >&2
	exit 1
	;;
esac

if [ -x "${SOURCE_DIR}/nativepipe-guestd" ]; then
	BIN="${SOURCE_DIR}/nativepipe-guestd"
elif [ -x "${SOURCE_DIR}/dist/${BIN_NAME}" ]; then
	BIN="${SOURCE_DIR}/dist/${BIN_NAME}"
else
	echo "error: guestd binary not found (build with: make -C guest/guestd)" >&2
	exit 1
fi

exec "${BIN}" --provision
