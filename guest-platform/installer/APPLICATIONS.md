# Installing applications by name

An installation plan sends names only, for example:

```json
"software": ["developer-tools", "codex", "claudecode", "chatgpt-desktop", "claude-desktop"]
```

On an installed VM, the same component is available as:

```sh
nativepipe-install apps --list
sudo nativepipe-install apps git python codex claudecode
sudo nativepipe-install apps chatgpt-desktop claude-desktop
```

`--list` needs no root permission and lists only applications supported by the
running distribution. `claude-code` is an alias for `claudecode`; `chatgpt` is an
alias for `chatgpt-desktop`. A request is validated in full before any package or
repository changes. Unsupported names, duplicate aliases and unsupported distro
combinations fail. Repeat the same command to ask the package manager to install
or update the applications; standalone CLIs select the current official release.
The installer does not log in, launch an authenticated session, or access secrets.

## Application table

`applications.json` is the single application policy. The following mapping
applies to the release series supported by the operating-system catalog.

| Application name | Alpine / APK | Debian, Ubuntu / APT | Fedora / DNF | Arch / Pacman |
| --- | --- | --- | --- | --- |
| `developer-tools` | build-base curl git | build-essential curl git | gcc gcc-c++ make cmake curl git | base-devel curl git |
| `git` | git | git | git | git |
| `curl` | curl ca-certificates | curl ca-certificates | curl ca-certificates | curl ca-certificates |
| `python` | python3 py3-pip | python3 python3-pip python3-venv | python3 python3-pip | python python-pip |
| `nodejs` | nodejs npm | nodejs npm | nodejs npm | nodejs npm |
| `ripgrep` | ripgrep | ripgrep | ripgrep | ripgrep |
| `codex` | Official musl binary | Official musl binary | Official musl binary | Official musl binary |
| `claudecode` | Official signed APK repo | Official signed APT repo | Official signed RPM repo | Official signed native binary |
| `chatgpt-desktop` | Unavailable | Official signed APT repo | Official signed RPM repo | Official signed Pacman repo |
| `claude-desktop` | Unavailable | Official signed APT repo | Unavailable | Unavailable |

Codex and both desktop applications currently install the release served by the
publisher. Claude Code's repository/native recipes use its `stable` channel.
The CLI binaries are available system-wide. Package-managed updates and removals
remain owned by the distribution. Codex's standalone executable is
`/usr/local/bin/codex`; Arch's standalone Claude Code is `/usr/local/bin/claude`.
Alpine adds a POSIX launcher selecting the system ripgrep, including non-login
launches from LinPortal Apps. Standalone binary replacement is atomic. No additional application daemon is introduced.

## Module boundaries

- `distro.c` owns OS baseline packages and direct package-manager argv.
- `packages.c` owns package execution and installation-time DNS restoration.
  It also removes only the previously identified unused physical-machine
  packages from fresh Arch roots. Application installation on a running system
  never performs that cleanup or rewrites resolver configuration.
  Fresh or resumed Arch amd64 installations using Rosetta persist
  `DisableSandboxFilesystem` and `DisableSandboxSyscalls` in pacman.conf before
  invoking pacman: Rosetta cannot install either filter. This automatic policy
  has no UI switch or installation-plan parameter. DownloadUser and signature
  verification remain unchanged; ARM roots and native amd64 retain both filters.
  Later application commands use the same persisted pacman configuration.
  Retries also upgrade the previous installer-owned filesystem-only setting.
- `applications.json` maps names to packages, prerequisites and preset scripts.
  Its shared preset prerequisites include comparison utilities on minimal
  glibc systems; Alpine uses the corresponding BusyBox commands.
- `apps.c` validates the whole selection, serializes application installations,
  runs prerequisites, executes a preset, refreshes new repositories, and installs
  the requested packages. Every nonzero subprocess status stops the operation.
- `presets/` contains reviewed POSIX shell scripts for publisher-specific setup.
  Scripts do not invoke apt, dnf, apk or pacman to install packages. Debian and
  Ubuntu share APT presets. The scripts select downloads by the target userspace
  ABI passed by C, not `uname -m`.
- `embed-apps.py` embeds the table and scripts into the C executable. Network
  manifests cannot provide shell text or script paths. Script stdin is bounded
  to 4096 bytes. Official binary downloads require SHA256 or signed manifests;
  repository keys are checked against recorded fingerprints before trust.
- `adapters/sync-apps.py` derives GUI choices from the same policy. Run it with
  `--write` after changing visible applications; `adapters/validate.py` detects
  drift. LinPortal transports only names and retains its existing host/guest
  installation protocol.

Recovery uses private mount/PID namespaces and restores DNS before every
package operation and preset. The existing resolver-after-postinst fix is
preserved. Optional software runs before final service/DNS configuration and
before handing off to the installed init. Failures leave the original recovery
installation state resumable.

The static, kernel-native recovery installer is retained at
`/usr/sbin/nativepipe-install` for later application commands. Even on an amd64
root it remains an ARM64 control helper; `/bin/sh`, package managers, installed
applications and guest services use the selected root ABI. Running-system
commands parse `/etc/os-release` as data and read `/bin/sh`'s ELF architecture.
They keep the live system's mounts, resolver and service-manager behavior.

Application package installation is not transactional across multiple names.
If a later application fails, previously installed applications stay installed;
retry after correcting the reported error. Existing modified repository files
are not silently replaced. Signing-key changes require a reviewed preset update.
For translated systemd roots, the installer also installs the narrow syscall
compatibility library and service settings documented in
[`rosetta-compat/README.md`](../rosetta-compat/README.md). Pacman's automatic
configuration separately addresses Landlock ENOSYS and seccomp EINVAL; signature
verification and DownloadUser remain enabled. See the amd64 root verification
report for actual installation and boot test results.

## Official installation references (checked 2026-09-30)

- [Codex CLI and standalone release assets](https://github.com/openai/codex#installing-and-running-codex-cli)
- [Claude Code packages and signature verification](https://code.claude.com/docs/en/setup)
- [ChatGPT desktop for Linux](https://learn.chatgpt.com/docs/linux/linux-app)
- [OpenAI's Arch bootstrap and repository signing key](https://persistent.oaistatic.com/codex-app-prod/linux/install-arch.sh)
- [Claude Desktop for Linux](https://support.claude.com/en/articles/10065433-install-claude-desktop)

Desktop availability here means an official package recipe exists. See the
LinPortal application-installer verification report for actual test outcomes;
account-dependent features and nested virtualization are separate requirements.
