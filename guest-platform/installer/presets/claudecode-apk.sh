fetch https://downloads.claude.ai/keys/claude-code.rsa.pub "$work/key.pub"
printf '%s  %s\n' 395759c1f7449ef4cdef305a42e820f3c766d6090d142634ebdb049f113168b6 "$work/key.pub" | sha256sum -c -
install -D -m 0644 "$work/key.pub" /etc/apk/keys/claude-code.rsa.pub
repo=https://downloads.claude.ai/claude-code/apk/stable
if ! grep -qxF "$repo" /etc/apk/repositories; then printf '\n%s\n' "$repo" >> /etc/apk/repositories; fi
# The official APK contains the binary directly. A POSIX launcher also covers
# non-login launches from FluxApps, where /etc/profile.d is not evaluated.
config /usr/local/bin/claude <<'LAUNCHER'
#!/bin/sh
export USE_BUILTIN_RIPGREP=0
exec /usr/bin/claude "$@"
LAUNCHER
chmod 0755 /usr/local/bin/claude
