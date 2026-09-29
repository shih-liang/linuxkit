fetch https://downloads.claude.ai/keys/claude-code.asc "$work/key.asc"
verify_key "$work/key.asc" 31DDDE24DDFAB679F42D7BD2BAA929FF1A7ECACE
install -D -m 0644 "$work/key.asc" /etc/pki/rpm-gpg/claude-code.asc
config /etc/yum.repos.d/claude-code.repo <<'REPO'
[claude-code]
name=Claude Code
baseurl=https://downloads.claude.ai/claude-code/rpm/stable
enabled=1
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/claude-code.asc
REPO
