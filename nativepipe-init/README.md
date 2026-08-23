# nativepipe-init

`nativepipe-init` is the fixed PID 1 for the LightHouse install and repair
initramfs. Distribution-specific behavior is supplied by a read-only VirtioFS
share named `nativepipe-install`; the initramfs does not download or embed
distribution adapters.

The share must contain `adapter.sh` and may contain a source artifact at
`source`. The adapter receives one action (`install` or `repair`) and these
environment variables:

```text
NP_TARGET_ROOT=/newroot
NP_TARGET_DISK=/dev/vda
NP_SOURCE_PATH=/run/nativepipe/payload/source
NP_AUTOMATIC=0|1
```

Supported kernel arguments:

```text
nativepipe.mode=normal|install|repair|shell
nativepipe.root=/dev/vda2
nativepipe.disk=/dev/vda
nativepipe.payload_tag=nativepipe-install
nativepipe.adapter=/run/nativepipe/payload/adapter.sh
nativepipe.source=/run/nativepipe/payload/source
nativepipe.manual
```

After a successful install or repair action, `nativepipe-init` mounts the
selected root and executes `/sbin/init` through BusyBox `switch_root`. Any
failure opens a persistent emergency shell on the VM console.

The release publishes the bare arm64 kernel `Image` expected by
`VZLinuxBootLoader.kernelURL` and a gzip-compressed `newc` archive for
`VZLinuxBootLoader.initialRamdiskURL`. Virtualization maps the RAM disk into
guest memory; the LightHouse kernel's `CONFIG_RD_GZIP` support decompresses it.
