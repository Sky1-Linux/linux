#!/bin/bash
# Build and install mainline kernel for Orion O6
# Usage: ./scripts/build-install.sh [clean]

set -e

KERNEL_IMAGE="IMAGE-6.18-mainline-cix"
DTB_FILE="SKY1-ORION-O6-6.18-mainline-cix.DTB"
EFI_DIR="/boot/efi"
JOBS=$(nproc)

cd "$(dirname "$0")/.."

echo "=== Orion O6 Mainline Kernel Build ==="
echo "Working directory: $(pwd)"
echo "Using $JOBS parallel jobs"
echo

# Clean build if requested
if [ "$1" = "clean" ]; then
    echo ">>> Cleaning build..."
    make clean
    echo
fi

# Build kernel, modules, and device trees
echo ">>> Building kernel, modules, and DTBs..."
make -j$JOBS Image modules dtbs

echo
echo ">>> Installing modules..."
sudo make modules_install

echo
echo ">>> Copying kernel image to EFI..."
sudo cp arch/arm64/boot/Image "$EFI_DIR/$KERNEL_IMAGE"

echo ">>> Copying DTB to EFI..."
sudo cp arch/arm64/boot/dts/cix/sky1-orion-o6.dtb "$EFI_DIR/$DTB_FILE"

echo ">>> Syncing EFI filesystem..."
sync

# Verify the copy was successful by comparing file sizes
SRC_SIZE=$(stat -c%s arch/arm64/boot/Image)
DST_SIZE=$(stat -c%s "$EFI_DIR/$KERNEL_IMAGE")
if [ "$SRC_SIZE" != "$DST_SIZE" ]; then
    echo "ERROR: Kernel image size mismatch!"
    echo "  Source: $SRC_SIZE bytes"
    echo "  Dest:   $DST_SIZE bytes"
    exit 1
fi

SRC_SIZE=$(stat -c%s arch/arm64/boot/dts/cix/sky1-orion-o6.dtb)
DST_SIZE=$(stat -c%s "$EFI_DIR/$DTB_FILE")
if [ "$SRC_SIZE" != "$DST_SIZE" ]; then
    echo "ERROR: DTB file size mismatch!"
    echo "  Source: $SRC_SIZE bytes"
    echo "  Dest:   $DST_SIZE bytes"
    exit 1
fi

echo
echo "=== Build Complete ==="
echo "Kernel:  $EFI_DIR/$KERNEL_IMAGE"
echo "DTB:     $EFI_DIR/$DTB_FILE"
echo "Modules: /lib/modules/$(make -s kernelrelease)"
echo
ls -la "$EFI_DIR/$KERNEL_IMAGE" "$EFI_DIR/$DTB_FILE"
echo
echo "Verified: EFI files match build artifacts."
echo "Ready to reboot. Use menu entry 3 (dev) for latest build."
