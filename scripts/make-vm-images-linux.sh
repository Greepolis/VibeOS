#!/usr/bin/env bash
# Produce importable VibeOS VM images from a completed build tree.
#
#   esp.img   raw FAT16 UEFI EFI System Partition (boots in QEMU+OVMF)
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
ESP_MB=$(du -sm "$EFI_ROOT" | cut -f1)
# Half again, and never below the old 24: FAT needs room for its own tables and
# for the boot to write into, and the write-proof test does exactly that.
ESP_MB=$(( ESP_MB + ESP_MB / 2 + 8 ))
if [[ "$ESP_MB" -lt 24 ]]; then ESP_MB=24; fi
echo "[VM-IMG] EFI root is $(du -sm "$EFI_ROOT" | cut -f1) MiB; ESP sized at ${ESP_MB} MiB"
python3 "$SCRIPT_DIR/make_esp_image.py" "$EFI_ROOT" "$ESP" "$ESP_MB" || exit 1

if command -v qemu-img >/dev/null 2>&1; then
  echo "[VM-IMG] converting to VirtualBox (.vdi) and VMware (.vmdk)..."
  qemu-img convert -f raw -O vdi  "$ESP" "$ART/vibeos.vdi"
  qemu-img convert -f raw -O vmdk "$ESP" "$ART/vibeos.vmdk"
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
  python3 "$SCRIPT_DIR/make_ova.py" "$ESP" "$ART" || exit 1
  python3 "$SCRIPT_DIR/dev/check-vm-images.py" "$ART" || exit 1
fi

echo "[VM-IMG] done. Artifacts in $ART:"
ls -1 "$ART" | grep -E '^vibeos(\.(vdi|vmdk|iso|ova|vmx)|-disk1\.vmdk|_esp\.img)$' || true
