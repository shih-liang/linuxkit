.PHONY: platform-test platform-build kernel initramfs

platform-test:
	$(MAKE) -C guest-platform test

platform-build:
	$(MAKE) -C guest-platform build

kernel initramfs:
	@echo 'kernel and initramfs are built only by the release workflow' >&2
	@exit 1
