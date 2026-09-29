# Key published by the official Arch bootstrap, shared by OpenAI Linux repos.
fetch https://persistent.oaistatic.com/codex-app-prod/linux/arch/26.928.20755/repository-signing-key.gpg "$work/key.gpg"
verify_key "$work/key.gpg" 3BFA0E4AE8B8CC16A2D9BA684A3B4A566C4660E4
gpg --homedir "$work/gnupg" --batch --import "$work/key.gpg"
gpg --homedir "$work/gnupg" --batch --armor --export 3BFA0E4AE8B8CC16A2D9BA684A3B4A566C4660E4 > "$work/key.asc"
key=/etc/pki/rpm-gpg/RPM-GPG-KEY-chatgpt-3BFA0E4AE8B8CC16A2D9BA684A3B4A566C4660E4.asc
install -D -m 0644 "$work/key.asc" "$key"
config /etc/yum.repos.d/chatgpt.repo <<REPO
[openai-chatgpt]
name=ChatGPT
baseurl=https://persistent.oaistatic.com/codex-app-prod/linux/rpm/\$basearch
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=file://$key
REPO
