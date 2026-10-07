# Guest kernel and device support

LinPortal boots an ARM64 Linux kernel. The 16 KiB variant serves ARM64 roots;
the 4 KiB variant also serves AMD64 roots through Rosetta. The root filesystem
architecture does not change the kernel's drivers or turn ARM KVM into x86 KVM.

The release build merges the pinned Kata ARM64 fragments with
[`config-aarch64`](../config-aarch64) and, for 4 KiB, the
[Rosetta overlay](../config-rosetta-aarch64). Linux `olddefconfig` resolves the
dependencies before the workflow verifies the resulting configuration.
The table describes the configuration contract; access to a physical USB
device also requires macOS permission and successful Virtualization.framework
attachment. LinPortal's USB picker uses Apple's all-accessory matching, without
device-class, interface-class or vendor filters. A selected device passes through
as a whole device, including all interfaces of a composite accessory.
This picker policy does not change the guest's driver configuration. Being
offered by the picker does not establish that the installed guest kernel,
firmware or userspace software can operate that device.

## USB devices

UVC video and camera USB audio are new configuration inputs in this change.
They have not yet been delivered in a new kernel release or installed in an
existing VM. Configuration resolution and driver compilation verify a build
contract; they do not verify physical camera capture or microphone recording.

| Device | Kernel configuration | Other requirements or limits |
| --- | --- | --- |
| Standard UVC camera | Built-in UVC, V4L2, media controller, video buffer helpers and USB audio for an integrated microphone | Linux application or PipeWire camera support. Non-UVC vendor cameras may require other kernel or userspace drivers. Cameras must be exposed by macOS as claimable USB accessories; this does not provide a bridge for other built-in cameras. |
| Mass storage | Built-in USB storage, UAS and SCSI disk support | Guest filesystem tools and mount permission. |
| Keyboard, mouse and other HID | Built-in USB HID, generic HID, evdev and hidraw | Basic standards-compatible HID support does not imply vendor tablet, multitouch, game-controller or force-feedback support. Wacom, multitouch and joystick-specific drivers are not selected. Guest device permissions still apply. |
| Serial adapters | Built-in CDC ACM, generic USB serial, CP210x, FTDI and PL2303 | Other vendor serial drivers are not selected; generic USB serial is not a substitute for every vendor protocol. |
| Ethernet adapter | Built-in CDC Ethernet/NCM, RNDIS, AX88179/178A, LAN78xx, SMSC95xx and RTL8152/8153 | Guest network configuration. This does not include every USB modem or Ethernet chipset. |
| Cellular modem | CDC ACM can expose a compatible serial interface; WWAN, CDC MBIM, QMI WWAN and USB WDM are disabled | A serial interface alone does not provide a modem's packet-data connection. Vendor modem drivers, modem management and often firmware are separate requirements. |
| Printer | Built-in USB printer support | CUPS and the appropriate driver or userspace IPP-over-USB service. |
| Scanner | USB core is present; no scanner-specific kernel driver contract | Common scanners use SANE/libusb in userspace, plus device access permission. |
| Smart card reader | USB core is present; no CCID-specific kernel driver contract | Usually pcsc-lite and libccid in userspace, plus device access permission. |
| USB audio | Built-in standard USB Audio/MIDI driver, PCM, raw MIDI and hardware-dependent helpers | Covers standard USB audio devices, including a camera's USB microphone. Virtio sound is enabled separately, and macOS must authorize physical-device attachment. Vendor audio drivers and MIDI 2.0 are not selected. |
| Wi-Fi | WLAN and cfg80211 are disabled | A future implementation needs selected USB chipset drivers, firmware and guest network tools. USB pass-through itself does not require these features to be disabled. |
| Bluetooth | Bluetooth and rfkill are disabled | A future implementation needs selected USB Bluetooth drivers, firmware and BlueZ. |
| USB bus tracing | usbmon is disabled | A diagnostic limitation, not a requirement for USB camera or storage operation. |

Camera support deliberately leaves television, radio, SDR, PCI media devices,
embedded platform camera pipelines and automatic sensor/tuner selection off.
The release workflow checks UVC, USB audio and their resolved video-buffer,
PCM and media-controller dependencies in both page-size variants using
[`check-kernel-usb-video.py`](../scripts/check-kernel-usb-video.py).
The driver relationships are defined by the upstream
[media](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/drivers/media/Kconfig?h=linux-6.18.y),
[UVC](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/drivers/media/usb/uvc/Kconfig?h=linux-6.18.y)
and [USB audio](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/sound/usb/Kconfig?h=linux-6.18.y)
Kconfig files.

## VM and software features

| Feature | Configuration contract | Boundary |
| --- | --- | --- |
| VM devices | Built-in virtio block, network, console, RNG, vsock, balloon, input, sound, DRM GPU and virtio-fs | The VM configuration still determines which devices are exposed. Virtio GPU does not provide an arbitrary physical GPU driver or host GPU pass-through. |
| Containers | Namespaces, cgroups, BPF, seccomp, virtual network devices and packet filtering enabled | A container runtime and distribution configuration are userspace concerns. |
| Networking | IPv4/IPv6, bridge/VLAN, TUN/TAP, nftables, virtual interfaces and WireGuard enabled; MPTCP and CAN disabled | Ordinary VM networking and VPN/container networking have their kernel foundations. Multipath TCP and USB CAN adapters require additional configuration and, for CAN, selected adapter drivers. |
| Filesystems | ext4, Btrfs, XFS, F2FS, FAT/exFAT, NTFS3, ISO/UDF, EROFS, SquashFS, FUSE, overlay, NFS, CIFS, 9p and virtio-fs enabled | Mount helpers, servers, credentials and permissions remain distribution dependent. |
| Mac-formatted disks | HFS and HFS+ disabled; APFS is not a mainline Linux filesystem | An attached USB disk is not necessarily readable. FAT/exFAT are included; other formats need suitable userspace software or a different driver-delivery decision. |
| Device mapper and disk arrays | Device mapper, encryption, thin provisioning and snapshots enabled; MD RAID, DM RAID and DM cache disabled | LUKS and LVM userspace tools are still required. Attaching multiple disks does not supply kernel RAID support. |
| Verified storage | dm-verity and fs-verity disabled | Verified block images and fs-verity file validation are unavailable under this configuration. |
| Security | Landlock, SELinux, AppArmor, Yama, IMA/EVM and seccomp enabled | Policies are supplied by the guest system. Rosetta syscall support is separate from the ARM64 kernel configuration. |
| Nested virtualization | ARM64 KVM enabled | The Mac hardware, macOS and Virtualization.framework must support nested virtualization, and the VM option must be enabled. AMD64 programs translated by Rosetta do not acquire x86 hardware virtualization. |
| Guest sleep and hibernation | Linux suspend and hibernation disabled | Guest `systemctl suspend` or hibernation is separate from pausing, saving and restoring the VM through Virtualization.framework. This does not remove LinPortal's host-side VM lifecycle operations. |
| Physical platform buses | MMC, MTD, ATA, platform IOMMU, pinctrl and SPI disabled | Virtualization.framework does not expose the Mac's physical platform buses. Passing through a USB peripheral does not expose those buses. |
| Physical-device assignment | VFIO disabled | Virtualization.framework exposes the selected virtual devices and USB pass-through, not a general physical PCI pass-through bus. |
| Nested I/O acceleration | vhost, vhost-net, vhost-vsock and vDPA disabled; CI currently rejects all vhost/vDPA drivers | This is a configuration policy, not a limitation on which physical hardware VZ exposes. A guest acting as an ARM KVM hypervisor could use vhost for nested-VM I/O, but the current kernel lacks that acceleration; QEMU userspace backends remain a separate available approach. |
| 32-bit programs | ARM32 compatibility (`CONFIG_COMPAT`) disabled | ARM32 execution would need both kernel compatibility and suitable virtual CPU support. Rosetta supports AMD64 translation, not 32-bit x86; that is a separate userspace architecture boundary. |

## Driver delivery

All supported in-tree drivers are built into the kernel Image. The release
workflow rejects a resolved configuration containing `=m`; no separate
`/lib/modules` tree is delivered to the guest. Installing a distribution's
stock kernel modules cannot add a driver to this independently built kernel.

External module loading remains enabled, and the build checks an external
module against the kernel it just built. Release artifacts include the Image,
resolved config, `System.map`, `Module.symvers` and source/license provenance.
They do not include a ready-made kernel headers package. Building an external
driver requires the exact kernel source, configuration, prepared build tree
and matching symbol versions; merely enabling `CONFIG_MODULES` is insufficient.

Firmware loading is enabled, but `CONFIG_EXTRA_FIRMWARE` is empty and the kernel
release does not ship a vendor firmware bundle. Fresh installer-owned Arch
roots remove the generic physical-machine kernel and `linux-firmware*` packages;
the supported virtio devices and standard UVC/USB audio classes do not require
those packages. Adding a firmware-dependent USB chipset would need an explicit
guest firmware delivery policy as well as its kernel driver. A driver being
built in does not prove such a device is usable.

Changing a configuration input prevents reuse of an earlier kernel release.
Until a new Image is built, published and selected by a VM, changing the source
configuration does not change the running guest kernel.
