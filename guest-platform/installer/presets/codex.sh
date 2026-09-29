# OpenAI's official standalone release; select the root ABI, never uname -m.
asset="codex-$machine-unknown-linux-musl"
fetch https://api.github.com/repos/openai/codex/releases/latest "$work/release.json"
url=$(jq -er --arg name "$asset.tar.gz" '.assets[] | select(.name==$name) | .browser_download_url' "$work/release.json")
digest=$(jq -er --arg name "$asset.tar.gz" '.assets[] | select(.name==$name) | .digest' "$work/release.json")
case "$url" in https://github.com/openai/codex/releases/download/*) ;; *) exit 1;; esac
sum=${digest#sha256:}
test "$digest" != "$sum" && test "${#sum}" = 64
case "$sum" in *[!0-9a-f]*) exit 1;; esac
fetch "$url" "$work/codex.tar.gz"
printf '%s  %s\n' "$sum" "$work/codex.tar.gz" | sha256sum -c -
# Extract only the documented binary, not arbitrary archive paths.
tar -xzf "$work/codex.tar.gz" -C "$work" "$asset"
chmod 0755 "$work/$asset"
"$work/$asset" --version
install -d -m 0755 /usr/local/bin
install -m 0755 "$work/$asset" /usr/local/bin/.nativepipe-codex.new
mv -f /usr/local/bin/.nativepipe-codex.new /usr/local/bin/codex
