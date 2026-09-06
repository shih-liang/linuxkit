#!/bin/sh
# Test account data and adapter commands against disposable text fixtures only.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/fluxwindow-account.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

for admin_group in sudo wheel; do
    (
    root="$work/$admin_group"
    mkdir -p "$root/etc" "$root/usr/sbin" "$root/usr/lib/systemd/system"
    printf 'root:x:0:0::/root:/bin/bash\nalarm:x:1000:1000::/home/alarm:/bin/bash\n' > "$root/etc/passwd"
    printf '%s:x:10:\naudio:x:11:\n' "$admin_group" > "$root/etc/group"
    for command in useradd usermod chpasswd; do
        touch "$root/usr/sbin/$command"
        chmod +x "$root/usr/sbin/$command"
    done
    touch "$root/usr/lib/systemd/system/serial-getty@.service"
    NP_SOURCE_PATH="$root/source"
    NP_TARGET_ROOT="$root"
    . "$repo/adapters/common.sh"
    # Never mount or modify a real system in this test.
    cleanup_target_root() { :; }
    run_in_target() {
        target=$1 command=${2##*/}
        shift 2
        case "$command" in
        chpasswd) IFS= read -r received; printf '%s\n' "$received" > "$target/received" ;;
        *) printf '%s %s\n' "$command" "$*" >> "$target/commands" ;;
        esac
    }
    printf '%s\n' alice 'literal:$HOME;$(not-a-command)\backslash' > "$root/account"
    load_installation_account
    create_installation_user "$root"
    test "$(cat "$root/received")" = 'alice:literal:$HOME;$(not-a-command)\backslash'
    grep -q '^useradd -m -s /bin/bash alice$' "$root/commands"
    grep -q '^usermod -L root$' "$root/commands"
    grep -q '^usermod -L alarm$' "$root/commands"
    grep -q "^usermod -aG $admin_group,audio alice$" "$root/commands"
    grep -q '^alice ALL=(ALL:ALL) ALL$' "$root/etc/sudoers.d/90-fluxwindow-user"
    test ! -e "$root/etc/systemd/system/serial-getty@hvc0.service.d/autologin.conf"
    test -z "${install_password+x}"
    for username in root '-root' 'a:b' 'bad user' ''; do
        printf '%s\n' "$username" example > "$root/account"
        if (load_installation_account) 2>/dev/null; then
            echo 'invalid account accepted' >&2
            exit 1
        fi
    done
    )
done
echo 'Installation account validation, literal password transport and login policy passed'
