#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Produce importable VibeOS VM images from a completed build tree.
#
#   esp.img   raw FAT16 UEFI EFI System Partition (boots in QEMU+OVMF)
#   disk.img  raw boot disk: MBR, the ESP and the ext4 root (the .vdi/.vmdk/.ova)
#   .vdi      VirtualBox disk (qemu-img)
#   .vmdk     VMware disk (qemu-img)
#   .iso      UEFI El Torito CD image (xorriso) - the most portable option
#
# esp/vdi/vmdk need only python3 + qemu-img. The .iso step is skipped with a
# notice if xorriso is unavailable (CI installs it). See README for how to
# import into VirtualBox / VMware (UEFI/EFI must be enabled in the VM).
#
# Usage: make-vm-images-linux.sh [build_dir]
set -uo pipefail

BUILD_DIR="${1:-build}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ART="$BUILD_DIR/artifacts"
EFI_ROOT="$ART/efi_root"
ESP="$ART/vibeos_esp.img"

if [[ ! -d "$EFI_ROOT" ]]; then
  echo "[VM-IMG] ERROR: EFI root not found at $EFI_ROOT (build the kernel image first)" >&2
  exit 1
fi

echo "[VM-IMG] building ESP image..."
# Sized from what is actually there, not from a number somebody chose once.
#
# It was a fixed 24 MB, and I5 put four filesystem test images on the medium -
# NTFS and exFAT are 16 MiB each - so the tree outgrew it and this step failed
# with nothing in the log that named the size. A constant that has to track the
# contents of a directory will stop tracking it; measuring costs one du.
#
# --apparent-size, because the filesystem images are made with truncate and are
# sparse: plain du counts the blocks they occupy, not the bytes the ESP writer
# copies. It measured 26 MiB for a tree that needed about 58 as soon as CI built
# the NTFS and exFAT images (2026-09-28), and the step failed with "content does
# not fit in image".
ESP_MB=$(du -sm --apparent-size "$EFI_ROOT" | cut -f1)
# Half again, and never below the old 24: FAT needs room for its own tables and
# for the boot to write into, and the write-proof test does exactly that.
ESP_MB=$(( ESP_MB + ESP_MB / 2 + 8 ))
if [[ "$ESP_MB" -lt 24 ]]; then ESP_MB=24; fi
echo "[VM-IMG] EFI root is $(du -sm --apparent-size "$EFI_ROOT" | cut -f1) MiB; ESP sized at ${ESP_MB} MiB"
python3 "$SCRIPT_DIR/make_esp_image.py" "$EFI_ROOT" "$ESP" "$ESP_MB" || exit 1

# The disk a machine boots: an MBR, the ESP and the native root on ext4 beside
# it, as the boot gate's disk is (docs/filesystem-design/ext4.md, E4-025). It
# used to be the bare ESP, so an imported VM ran with no root at all. Without
# mkfs.ext4 there is no root to make, and that is an error here: an image
# without one is not the system.
#
# It is a production image (E4-023, E4-022): the root carries no test programs
# and no fixtures, and init starts the shell rather than the boot gate's
# self-test; the ESP carries what the firmware and the kernel read from it -
# the loader, the kernel, INIT.ELF for a disk with no root, the swap file - and
# not the programs or the filesystem test images. The .iso above is still the
# full ESP: a CD holds no root partition, so it is the one medium where the
# ESP has to be the whole system.
DISK="$ART/vibeos_disk.img"
ROOT_TREE="$ART/root_tree"
PROD_EFI="$ART/efi_root_production"
PROD_ESP="$ART/vibeos_esp_production.img"
rm -rf "$ROOT_TREE" "$PROD_EFI"
python3 "$SCRIPT_DIR/make-root-tree.py" "$EFI_ROOT" "$ROOT_TREE" --production || exit 1
mkdir -p "$PROD_EFI/EFI/BOOT"
for f in BOOTX64.EFI VIBEOSKR.ELF INIT.ELF SWAPFILE.BIN; do
  [ -f "$EFI_ROOT/EFI/BOOT/$f" ] && cp "$EFI_ROOT/EFI/BOOT/$f" "$PROD_EFI/EFI/BOOT/"
done
[ -f "$EFI_ROOT/startup.nsh" ] && cp "$EFI_ROOT/startup.nsh" "$PROD_EFI/"
PROD_ESP_MB=$(du -sm --apparent-size "$PROD_EFI" | cut -f1)
PROD_ESP_MB=$(( PROD_ESP_MB + PROD_ESP_MB / 2 + 8 ))
if [[ "$PROD_ESP_MB" -lt 24 ]]; then PROD_ESP_MB=24; fi
python3 "$SCRIPT_DIR/make_esp_image.py" "$PROD_EFI" "$PROD_ESP" "$PROD_ESP_MB" || exit 1
ROOT_MB=$(du -sm --apparent-size "$ROOT_TREE" | cut -f1)
ROOT_MB=$(( ROOT_MB * 2 + 256 ))
echo "[VM-IMG] building the boot disk: ESP ${PROD_ESP_MB} MiB, root ${ROOT_MB} MiB (ext4), production..."
python3 "$SCRIPT_DIR/make-boot-disk.py" make "$PROD_ESP" "$ROOT_TREE" "$DISK" "$ROOT_MB"
rc=$?
rm -rf "$ROOT_TREE" "$PROD_EFI" "$PROD_ESP"
if [[ "$rc" -ne 0 ]]; then
  echo "[VM-IMG] ERROR: make-boot-disk.py failed (rc=$rc; 3 is no mkfs.ext4)" >&2
  exit 1
fi

if command -v qemu-img >/dev/null 2>&1; then
  echo "[VM-IMG] converting to VirtualBox (.vdi) and VMware (.vmdk)..."
  qemu-img convert -f raw -O vdi  "$DISK" "$ART/vibeos.vdi"
  qemu-img convert -f raw -O vmdk "$DISK" "$ART/vibeos.vmdk"
else
  echo "[VM-IMG] NOTE: qemu-img not found; skipping .vdi/.vmdk conversion"
fi

if command -v xorriso >/dev/null 2>&1; then
  echo "[VM-IMG] building UEFI El Torito .iso..."
  ISO_ROOT="$ART/iso_root"
  rm -rf "$ISO_ROOT"
  mkdir -p "$ISO_ROOT"
  # Put the EFI tree directly in the ISO 9660 filesystem (so /EFI/BOOT exists
  # for firmware that reads the ISO FS) *and* embed the ESP as the El Torito
  # EFI boot image. This hybrid layout maximizes VirtualBox/VMware compat.
  cp -r "$EFI_ROOT/EFI" "$ISO_ROOT/"
  [ -f "$EFI_ROOT/startup.nsh" ] && cp "$EFI_ROOT/startup.nsh" "$ISO_ROOT/"
  cp "$ESP" "$ISO_ROOT/vibeos_esp.img"
  xorriso -as mkisofs \
    -V VIBEOS -R -J -f \
    -e vibeos_esp.img -no-emul-boot \
    -o "$ART/vibeos.iso" "$ISO_ROOT" || exit 1
  rm -rf "$ISO_ROOT"
else
  echo "[VM-IMG] NOTE: xorriso not found; skipping .iso (install xorriso to enable)"
fi

# The disks above are disks, not machines: importing one still leaves you
# creating a VM, picking a chipset and - the step everybody misses - turning on
# EFI. The appliance carries that configuration with it.
if command -v qemu-img >/dev/null 2>&1; then
  echo "[VM-IMG] building importable appliance (.ova) and VMware .vmx..."
  python3 "$SCRIPT_DIR/make_ova.py" "$DISK" "$ART" || exit 1
  python3 "$SCRIPT_DIR/dev/check-vm-images.py" "$ART" || exit 1
fi

echo "[VM-IMG] done. Artifacts in $ART:"
ls -1 "$ART" | grep -E '^vibeos(\.(vdi|vmdk|iso|ova|vmx)|-disk1\.vmdk|_(esp|disk)\.img)$' || true
