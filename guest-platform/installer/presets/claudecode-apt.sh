fetch https://downloads.claude.ai/keys/claude-code.asc "$work/key.asc"
verify_key "$work/key.asc" 31DDDE24DDFAB679F42D7BD2BAA929FF1A7ECACE
install -D -m 0644 "$work/key.asc" /etc/apt/keyrings/claude-code.asc
config /etc/apt/sources.list.d/claude-code.list <<'REPO'
deb [signed-by=/etc/apt/keyrings/claude-code.asc] https://downloads.claude.ai/claude-code/apt/stable stable main
REPO
