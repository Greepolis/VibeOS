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
python3 "$SCRIPT_DIR/make_esp_image.py" "$EFI_ROOT" "$ESP" 48 || exit 1

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
  cp "$ESP" "$ISO_ROOT/vibeos_esp.img"
  xorriso -as mkisofs \
    -V VIBEOS -R -f \
    -e vibeos_esp.img -no-emul-boot \
    -o "$ART/vibeos.iso" "$ISO_ROOT" || exit 1
  rm -rf "$ISO_ROOT"
else
  echo "[VM-IMG] NOTE: xorriso not found; skipping .iso (install xorriso to enable)"
fi

echo "[VM-IMG] done. Artifacts in $ART:"
ls -1 "$ART" | grep -E '^vibeos(\.(vdi|vmdk|iso)|_esp\.img)$' || true
