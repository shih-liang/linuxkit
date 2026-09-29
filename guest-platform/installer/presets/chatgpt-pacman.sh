# Key published by the official Arch bootstrap, shared by OpenAI Linux repos.
fetch https://persistent.oaistatic.com/codex-app-prod/linux/arch/26.928.20755/repository-signing-key.gpg "$work/key.gpg"
verify_key "$work/key.gpg" 3BFA0E4AE8B8CC16A2D9BA684A3B4A566C4660E4
pacman-key --add "$work/key.gpg"
pacman-key --lsign-key 3BFA0E4AE8B8CC16A2D9BA684A3B4A566C4660E4
config /etc/pacman.d/openai-chatgpt.conf <<'REPO'
### THIS FILE IS AUTOMATICALLY CONFIGURED ###
# Remove it and its Include line to opt out of automatic package updates.
[openai-chatgpt]
SigLevel = Required DatabaseRequired TrustedOnly
Server = https://persistent.oaistatic.com/codex-app-prod/linux/arch/$arch
REPO
include='Include = /etc/pacman.d/openai-chatgpt.conf'
if ! grep -qxF "$include" /etc/pacman.conf; then printf '\n%s\n' "$include" >> /etc/pacman.conf; fi
