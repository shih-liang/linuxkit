# Key published by the official Arch bootstrap, shared by OpenAI Linux repos.
fetch https://persistent.oaistatic.com/codex-app-prod/linux/arch/26.928.20755/repository-signing-key.gpg "$work/key.gpg"
verify_key "$work/key.gpg" 3BFA0E4AE8B8CC16A2D9BA684A3B4A566C4660E4
install -D -m 0644 "$work/key.gpg" /usr/share/keyrings/chatgpt-archive-keyring.gpg
config /etc/apt/sources.list.d/chatgpt.sources <<REPO
### THIS FILE IS AUTOMATICALLY CONFIGURED ###
# Remove it to opt out of automatic package updates.
X-Repolib-Name: ChatGPT
Types: deb
URIs: https://persistent.oaistatic.com/codex-app-prod/linux/deb
Suites: stable
Components: main
Architectures: $architecture
Signed-By: /usr/share/keyrings/chatgpt-archive-keyring.gpg
REPO
