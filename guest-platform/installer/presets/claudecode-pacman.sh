# Anthropic has no pacman repository. Use its signed native release instead.
base=https://downloads.claude.ai/claude-code-releases
fetch "$base/stable" "$work/version"
version=$(cat "$work/version")
printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'
case "$architecture" in arm64) platform=linux-arm64;; amd64) platform=linux-x64;; esac
fetch https://downloads.claude.ai/keys/claude-code.asc "$work/key.asc"
verify_key "$work/key.asc" 31DDDE24DDFAB679F42D7BD2BAA929FF1A7ECACE
gpg --homedir "$work/gnupg" --batch --import "$work/key.asc"
fetch "$base/$version/manifest.json" "$work/manifest.json"
fetch "$base/$version/manifest.json.sig" "$work/manifest.json.sig"
gpg --homedir "$work/gnupg" --batch --no-auto-key-retrieve --no-auto-key-import --verify "$work/manifest.json.sig" "$work/manifest.json"
sum=$(jq -er --arg platform "$platform" '.platforms[$platform].checksum' "$work/manifest.json")
test "${#sum}" = 64
case "$sum" in *[!0-9a-f]*) exit 1;; esac
fetch "$base/$version/$platform/claude" "$work/claude"
printf '%s  %s\n' "$sum" "$work/claude" | sha256sum -c -
chmod 0755 "$work/claude"
"$work/claude" --version
install -d -m 0755 /usr/local/bin
install -m 0755 "$work/claude" /usr/local/bin/.nativepipe-claude.new
mv -f /usr/local/bin/.nativepipe-claude.new /usr/local/bin/claude
