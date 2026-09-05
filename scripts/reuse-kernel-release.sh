#!/bin/bash
# Reuse a kernel only when its Linux version and configuration inputs match.
set -euo pipefail
tag=${1:?verified kernel release is required}
: "${GH_REPO:?GitHub repository is required}"
: "${KERNEL_VERSION:?kernel version is required}"
: "${KATA_CONFIG_REF:?Kata configuration version is required}"
[[ "$tag" == lighthouse-kernel-v* ]]
gh api "repos/$GH_REPO/releases/tags/$tag" --jq '.draft' | grep -qx false
mkdir -p reused-kernel-source artifact
for name in KERNEL_VERSION DEPENDENCY_VERSIONS config-aarch64; do
    gh api "repos/$GH_REPO/contents/$name?ref=$tag" --jq .content \
        | base64 --decode > "reused-kernel-source/$name"
done
test "$(tr -d '[:space:]' < reused-kernel-source/KERNEL_VERSION)" = "$KERNEL_VERSION"
grep -Fxq "KATA_CONFIG_REF=$KATA_CONFIG_REF" reused-kernel-source/DEPENDENCY_VERSIONS
cmp config-aarch64 reused-kernel-source/config-aarch64
image="lighthouse-linux-${KERNEL_VERSION}-arm64-16k.Image"
gh release download "$tag" --repo "$GH_REPO" --dir artifact \
    --pattern "$image" --pattern config-aarch64 --pattern System.map \
    --pattern Module.symvers --pattern SHA256SUMS
(
    cd artifact
    for name in "$image" config-aarch64 System.map Module.symvers; do
        grep -F "  $name" SHA256SUMS | grep -E "  $name$"
    done | sha256sum --check --strict -
)
rm artifact/SHA256SUMS
echo "Reused verified kernel from $tag; no Linux compilation"
