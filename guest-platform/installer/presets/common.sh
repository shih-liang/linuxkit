# Embedded in nativepipe-install; only the compiled application table selects it.
set -eu
architecture=$1
distribution=$2
case "$architecture" in arm64) machine=aarch64;; amd64) machine=x86_64;; *) exit 64;; esac
work=$(mktemp -d /tmp/nativepipe-app.XXXXXXXX)
trap 'rm -rf "$work"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
fetch() { curl --proto '=https' --proto-redir '=https' --tlsv1.2 -fsSL --retry 2 --retry-all-errors --connect-timeout 20 --max-time 600 -o "$2" "$1"; }
# Check a key in a private keyring, before importing it or trusting a repository.
verify_key() {
    mkdir -p "$work/gnupg"
    chmod 700 "$work/gnupg"
    actual=$(gpg --homedir "$work/gnupg" --batch --show-keys --with-colons "$1" | awk -F: '$1=="pub"{primary=1} $1=="sub"{primary=0} $1=="fpr"&&primary{print $10;primary=0}')
    test "$actual" = "$2" || { echo 'Repository signing key mismatch' >&2; exit 1; }
}
# Do not replace repository settings edited by the user. Repeated installation
# with the same preset is harmless. Package managers retain their own locks.
config() {
    target=$1
    cat > "$work/config"
    if test -e "$target"; then
        cmp -s "$work/config" "$target" || { echo "Existing repository differs: $target" >&2; exit 1; }
    else
        install -D -m 0644 "$work/config" "$target"
    fi
}
