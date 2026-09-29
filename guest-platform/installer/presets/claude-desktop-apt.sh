fetch https://downloads.claude.ai/claude-desktop/key.asc "$work/key.asc"
verify_key "$work/key.asc" 31DDDE24DDFAB679F42D7BD2BAA929FF1A7ECACE
install -D -m 0644 "$work/key.asc" /usr/share/keyrings/claude-desktop-archive-keyring.asc
config /etc/apt/sources.list.d/claude-desktop.list <<'REPO'
deb [signed-by=/usr/share/keyrings/claude-desktop-archive-keyring.asc] https://downloads.claude.ai/claude-desktop/apt/stable stable main
REPO
