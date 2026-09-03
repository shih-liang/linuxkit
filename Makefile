ARTIFACTS_ROOT ?= $(abspath ../artifacts)
RELEASE_TAG ?=
DIST_DIR ?= $(CURDIR)/guest-platform/dist

.PHONY: platform-test platform-build platform-package stage-platform kernel initramfs

platform-test:
	$(MAKE) -C guest-platform test

platform-build:
	$(MAKE) -C guest-platform build

platform-package:
	$(MAKE) -C guest-platform package

# Stage only the signed guest-platform release. Kernel and initramfs releases
# are downloaded and managed at runtime; they never enter FluxWindow.app.
stage-platform:
	@test -n "$(RELEASE_TAG)" || { echo 'RELEASE_TAG is required' >&2; exit 1; }
	./scripts/stage-release-assets.sh "$(DIST_DIR)" "$(ARTIFACTS_ROOT)" \
		shih-liang/linuxkit "$(RELEASE_TAG)" linuxkit-platform

kernel initramfs:
	@echo 'kernel and initramfs are built only by the release workflow' >&2
	@exit 1
