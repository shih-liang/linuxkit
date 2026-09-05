#!/bin/sh
set -eu

prefix_length()
{
	old_ifs=$IFS
	IFS=.
	set -- $1
	IFS=$old_ifs
	[ "$#" -eq 4 ] || return 1
	prefix=0 complete=0
	for octet do
		[ "$complete" -eq 0 ] || [ "$octet" -eq 0 ] || return 1
		case "$octet" in
		255) bits=8 ;; 254) bits=7 ;; 252) bits=6 ;; 248) bits=5 ;;
		240) bits=4 ;; 224) bits=3 ;; 192) bits=2 ;; 128) bits=1 ;;
		0) bits=0 ;; *) return 1 ;;
		esac
		prefix=$((prefix + bits))
		[ "$bits" -eq 8 ] || complete=1
	done
	printf '%s\n' "$prefix"
}

if [ "${1:-}" = --self-test ]; then
	[ "$(prefix_length 255.255.255.0)" = 24 ]
	[ "$(prefix_length 255.255.254.0)" = 23 ]
	! prefix_length 255.0.255.0 >/dev/null 2>&1
	exit 0
fi

: "${interface:?udhcpc did not provide an interface}"
ip link set dev "$interface" up
case ${1:-} in
deconfig)
	ip -4 addr flush dev "$interface"
	;;
bound|renew)
	: "${ip:?udhcpc did not provide an address}"
	prefix=$(prefix_length "${subnet:-255.255.255.0}")
	ip -4 addr flush dev "$interface"
	ip -4 addr add "$ip/$prefix" dev "$interface"
	ip -4 route del default dev "$interface" 2>/dev/null || true
	set -- ${router:-}
	[ "$#" -eq 0 ] || ip -4 route add default via "$1" dev "$interface"
	: > /etc/resolv.conf
	[ -z "${domain:-}" ] || printf 'search %s\n' "$domain" >> /etc/resolv.conf
	for server in ${dns:-}; do
		printf 'nameserver %s\n' "$server" >> /etc/resolv.conf
	done
	;;
esac
