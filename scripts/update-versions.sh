#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repository="$(cd "$script_dir/.." && pwd)"
cd "$repository"

source DEPENDENCY_VERSIONS
current_kernel="$(tr -d '[:space:]' < KERNEL_VERSION)"

fetch() {
    local url="$1"
    local token="${GITHUB_TOKEN:-${GH_TOKEN:-}}"
    local -a headers=(--user-agent 'LightHouse dependency updater')

    if [[ "$url" == https://api.github.com/* && -n "$token" ]]; then
        headers+=(
            --header "Authorization: Bearer $token"
            --header 'Accept: application/vnd.github+json'
            --header 'X-GitHub-Api-Version: 2022-11-28'
        )
    fi

    curl --http1.1 --fail --location --silent --show-error \
        --connect-timeout 10 --max-time 90 \
        --retry 5 --retry-delay 3 --retry-max-time 180 --retry-all-errors \
        "${headers[@]}" "$url"
}

require_version() {
    local name="$1"
    local version="$2"
    if ! grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' <<<"$version"; then
        echo "$name returned an invalid version: $version" >&2
        exit 1
    fi
}

require_release_version() {
    local name="$1"
    local version="$2"
    if ! grep -Eq '^[0-9]+\.[0-9]+(\.[0-9]+)?$' <<<"$version"; then
        echo "$name returned an invalid version: $version" >&2
        exit 1
    fi
}

refuse_downgrade() {
    local name="$1"
    local current="$2"
    local latest="$3"
    if [[ "$(printf '%s\n%s\n' "$current" "$latest" | sort -V | tail -n 1)" != "$latest" ]]; then
        echo "Refusing to downgrade $name from $current to $latest" >&2
        exit 1
    fi
}

latest_kernel="$({
    fetch https://www.kernel.org/releases.json \
        | jq -r '.releases[] | select(.moniker == "longterm" and .iseol == false) | .version' \
        | sort -V \
        | tail -n 1
})"

# BusyBox labels x.y.0 development releases "unstable" and bug-fix releases
# x.y.N (N > 0) "stable". Its homepage links this maintainer mirror for source
# browsing; use its tags so a slow busybox.net cannot stall every weekly check.
latest_busybox="$({
    fetch 'https://api.github.com/repos/vda-linux/busybox_mirror/tags?per_page=100' \
        | jq -r '.[].name' \
        | grep -E '^1_[0-9]+_[1-9][0-9]*$' \
        | tr '_' '.' \
        | sort -V \
        | tail -n 1
})"

latest_e2fsprogs="$({
    fetch https://www.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/ \
        | grep -Eo 'href="v[0-9]+\.[0-9]+\.[0-9]+/"' \
        | sed -E 's/^href="v|\/"$//g' \
        | sort -V \
        | tail -n 1
})"

latest_util_series="$({
    fetch https://www.kernel.org/pub/linux/utils/util-linux/ \
        | grep -Eo 'href="v[0-9]+\.[0-9]+/"' \
        | sed -E 's/^href="v|\/"$//g' \
        | sort -V \
        | tail -n 1
})"
latest_util_linux="$({
    fetch "https://www.kernel.org/pub/linux/utils/util-linux/v${latest_util_series}/" \
        | grep -Eo 'util-linux-[0-9]+\.[0-9]+(\.[0-9]+)?\.tar\.xz' \
        | sed -E 's/^util-linux-|\.tar\.xz$//g' \
        | sort -Vu \
        | tail -n 1
})"

latest_kata="$({
    fetch https://api.github.com/repos/kata-containers/kata-containers/releases/latest \
        | jq -r 'select(.draft == false and .prerelease == false) | .tag_name'
})"
latest_kata="${latest_kata#v}"

for pair in \
    "Linux:$latest_kernel" \
    "BusyBox:$latest_busybox" \
    "e2fsprogs:$latest_e2fsprogs" \
    "Kata:$latest_kata"; do
    require_version "${pair%%:*}" "${pair#*:}"
done
require_release_version util-linux "$latest_util_linux"

refuse_downgrade Linux "$current_kernel" "$latest_kernel"
refuse_downgrade BusyBox "$BUSYBOX_VERSION" "$latest_busybox"
refuse_downgrade e2fsprogs "$E2FSPROGS_VERSION" "$latest_e2fsprogs"
refuse_downgrade util-linux "$UTIL_LINUX_VERSION" "$latest_util_linux"
refuse_downgrade Kata "$KATA_CONFIG_REF" "$latest_kata"

busybox_archive="busybox-${latest_busybox}.tar.bz2"
busybox_sha256="$BUSYBOX_SHA256"
if [[ "$latest_busybox" != "$BUSYBOX_VERSION" ]]; then
    busybox_sha256="$({
        fetch "https://busybox.net/downloads/${busybox_archive}.sha256" \
            | grep -Eo '[0-9a-fA-F]{64}' \
            | head -n 1 \
            | tr 'A-F' 'a-f'
    })"
fi

e2fsprogs_archive="e2fsprogs-${latest_e2fsprogs}.tar.xz"
e2fsprogs_sha256="$E2FSPROGS_SHA256"
if [[ "$latest_e2fsprogs" != "$E2FSPROGS_VERSION" ]]; then
    e2fsprogs_sha256="$({
        fetch "https://www.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v${latest_e2fsprogs}/sha256sums.asc" \
            | awk -v archive="$e2fsprogs_archive" '$2 == archive { print $1; exit }'
    })"
fi

util_linux_archive="util-linux-${latest_util_linux}.tar.xz"
util_linux_sha256="$UTIL_LINUX_SHA256"
if [[ "$latest_util_linux" != "$UTIL_LINUX_VERSION" ]]; then
    util_linux_sha256="$({
        fetch "https://www.kernel.org/pub/linux/utils/util-linux/v${latest_util_series}/sha256sums.asc" \
            | awk -v archive="$util_linux_archive" '$2 == archive { print $1; exit }'
    })"
fi

for pair in \
    "BusyBox:$busybox_sha256" \
    "e2fsprogs:$e2fsprogs_sha256" \
    "util-linux:$util_linux_sha256"; do
    if ! grep -Eq '^[0-9a-f]{64}$' <<<"${pair#*:}"; then
        echo "${pair%%:*} returned an invalid SHA-256: ${pair#*:}" >&2
        exit 1
    fi
done

# Kata fragments are a base configuration, not a second kernel source. Refuse
# an automatic PR when that base targets a different kernel series; a human can
# then review the Kconfig migration instead of discovering it during release.
kata_versions="$(fetch "https://raw.githubusercontent.com/kata-containers/kata-containers/${latest_kata}/versions.yaml")"
kata_kernel="$({
    sed -n '/^  kernel:$/,/^  [^ ]/ {
        s/^    version: "v\([0-9][0-9.]*\)".*/\1/p
    }' <<<"$kata_versions" | head -n 1
})"
require_version "Kata kernel" "$kata_kernel"
if [[ "${latest_kernel%.*}" != "${kata_kernel%.*}" ]]; then
    echo "Kata ${latest_kata} targets Linux ${kata_kernel}, not ${latest_kernel}" >&2
    exit 1
fi

# Fail the update PR early if the newest Kata release no longer carries the
# configuration fragments consumed by the kernel build.
for scope in common arm64; do
    fetch "https://api.github.com/repos/kata-containers/kata-containers/contents/tools/packaging/kernel/configs/fragments/${scope}?ref=${latest_kata}" \
        | jq -e 'any(.[]; .type == "file" and (.name | endswith(".conf")))' >/dev/null
done

printf '%s\n' "$latest_kernel" > KERNEL_VERSION
printf '%s\n' \
    "BUSYBOX_VERSION=$latest_busybox" \
    "BUSYBOX_SHA256=$busybox_sha256" \
    "E2FSPROGS_VERSION=$latest_e2fsprogs" \
    "E2FSPROGS_SHA256=$e2fsprogs_sha256" \
    "UTIL_LINUX_VERSION=$latest_util_linux" \
    "UTIL_LINUX_SHA256=$util_linux_sha256" \
    "KATA_CONFIG_REF=$latest_kata" > DEPENDENCY_VERSIONS

if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    printf '%s\n' \
        "kernel=$latest_kernel" \
        "busybox=$latest_busybox" \
        "e2fsprogs=$latest_e2fsprogs" \
        "util_linux=$latest_util_linux" \
        "kata=$latest_kata" >> "$GITHUB_OUTPUT"
fi

printf 'Linux %s, BusyBox %s, e2fsprogs %s, util-linux %s, Kata %s\n' \
    "$latest_kernel" "$latest_busybox" "$latest_e2fsprogs" \
    "$latest_util_linux" "$latest_kata"
