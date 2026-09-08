#!/bin/sh
# Test account data and adapter commands against disposable text fixtures only.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/fluxwindow-account.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

for distro in ubuntu fedora archlinux; do
    (
    case "$distro" in
        ubuntu) admin_group=sudo; extra_group= ;;
        fedora) admin_group=wheel; extra_group= ;;
        archlinux) admin_group=wheel; extra_group=realtime ;;
    esac
    root="$work/$distro"
    mkdir -p "$root/etc" "$root/sbin" "$root/usr/sbin" "$root/usr/lib/systemd/system"
    touch "$root/sbin/init"
    chmod +x "$root/sbin/init"
    printf 'root:x:0:0::/root:/bin/bash\nalarm:x:1000:1000::/home/alarm:/bin/bash\n' > "$root/etc/passwd"
    printf '%s:x:10:\naudio:x:11:\n' "$admin_group" > "$root/etc/group"
    # An existing realtime group must not grant ordinary users extra privileges.
    # Only Arch explicitly opts in to its official package's policy.
    printf 'realtime:x:12:\n' >> "$root/etc/group"
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
    install_selected_software() { :; }
    configure_network() { :; }
    install_guest_agent() { :; }
    install_rosetta_support() { :; }
    sync() { :; }
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
    finish_rootfs "$extra_group"
    test "$(cat "$root/received")" = 'alice:literal:$HOME;$(not-a-command)\backslash'
    grep -q '^useradd -m -s /bin/bash alice$' "$root/commands"
    grep -q '^usermod -L root$' "$root/commands"
    grep -q '^usermod -L alarm$' "$root/commands"
    grep -q "^usermod -aG $admin_group,audio${extra_group:+,$extra_group} alice$" "$root/commands"
    grep -qx 'runuser -u alice -- /usr/bin/env SYSTEMD_OFFLINE=1 /usr/bin/systemctl --user --no-reload enable pipewire.service pipewire-pulse.service wireplumber.service' "$root/commands"
    if grep -q '^groupadd ' "$root/commands"; then
        echo 'account creation must not create a realtime group' >&2
        exit 1
    fi
    test ! -e "$root/etc/security/limits.d/99-fluxwindow-realtime.conf"
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
