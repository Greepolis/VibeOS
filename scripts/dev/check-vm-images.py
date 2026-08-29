#!/usr/bin/env python3
"""Check that the appliance we ship is actually importable.

    python3 scripts/dev/check-vm-images.py <artifacts-dir>

"Importable" is a promise about a file somebody else opens on a machine we do
not have, so it is worth checking rather than declaring. Everything asserted
here has a failure mode that is silent at import time and only shows up as a
guest that will not start:

  * the descriptor must be the first member of the archive - an importer reads
    it to learn what the rest of the tar holds, and finds nothing if the disk
    comes first;
  * the manifest hashes must match, or VMware refuses the appliance outright;
  * the firmware must say EFI in both dialects. This is the one that costs an
    evening: VibeOS has no legacy boot path, so a VM that imports cleanly onto
    default BIOS firmware shows a blank window and no error at all.

Booting the disk is checked separately by qemu-boot-image-linux.py, which is
the part this file deliberately does not duplicate.
"""
import hashlib
import os
import re
import sys
import tarfile
import xml.etree.ElementTree as ET

NS = {
    "ovf": "http://schemas.dmtf.org/ovf/envelope/1",
    "vbox": "http://www.virtualbox.org/ovf/machine",
    "vmw": "http://www.vmware.com/schema/ovf",
}


import uuid


def fail(problems, message):
    problems.append(message)


def check_ova(path, problems):
    with tarfile.open(path, "r") as tar:
        members = tar.getnames()
        if not members:
            fail(problems, "ova is empty")
            return
        if not members[0].endswith(".ovf"):
            fail(problems, f"first archive member is {members[0]}, not the .ovf "
                           "descriptor")

        # Carry on with the descriptor wherever it actually is. Reading
        # members[0] regardless would make this crash on a binary member, and a
        # check that fails by traceback fails for a reason other than the one
        # it names - which is worth less than no check at all.
        ovf_names = [m for m in members if m.endswith(".ovf")]
        if not ovf_names:
            fail(problems, "no .ovf descriptor in the archive")
            return
        ovf_member = ovf_names[0]
        ovf_text = tar.extractfile(ovf_member).read().decode("utf-8")

        try:
            root = ET.fromstring(ovf_text)
        except ET.ParseError as exc:
            fail(problems, f"ovf does not parse: {exc}")
            return

        ref = root.find("ovf:References/ovf:File", NS)
        if ref is None:
            fail(problems, "ovf references no disk file")
        else:
            href = ref.get("{%s}href" % NS["ovf"])
            declared = int(ref.get("{%s}size" % NS["ovf"], "0"))
            if href not in members:
                fail(problems, f"ovf references {href}, which is not in the ova")
            else:
                actual = tar.getmember(href).size
                if actual != declared:
                    fail(problems, f"ovf declares {href} as {declared} bytes, "
                                   f"archive holds {actual}")

        # VirtualBox refuses to import a machine with no uuid, and says so in
        # a message that reads as if it were about the name. Checked here
        # because the first appliance shipped without one and the failure only
        # appeared on somebody else's machine.
        machine_el = root.find(".//vbox:Machine", NS)
        if machine_el is None or not machine_el.get("uuid"):
            fail(problems, "vbox:Machine has no uuid; VirtualBox refuses the "
                           "import with 'Required Machine/@uuid ... is missing'")

        # The machine's disk attachment and the OVF's disk have to name the
        # same image, and they say so in two different notations: the Disk
        # element carries vbox:uuid bare, the AttachedDevice's Image carries
        # the same value in braces. Braces on both, or a uuid on only one side,
        # imports 0% of the way and stops with "<vbox:Machine> element in OVF
        # contains a medium attachment for the disk image ... but the OVF
        # describes no such image" - which names an image the OVF does
        # describe, one section earlier.
        #
        # Both were wrong at once here: the uuids were slices of a SHA-256
        # digest, so their version and variant nibbles were whatever the digest
        # happened to hold. VirtualBox parses such a value to null instead of
        # rejecting it, so the first failure named an empty image.
        disks = root.findall(".//{*}DiskSection/{*}Disk")
        images = root.findall(".//{*}AttachedDevice/{*}Image")
        if not images:
            fail(problems, "vbox:Machine attaches no disk; VirtualBox builds "
                           "storage from this section, not from the generic "
                           "OVF hardware items, so the guest boots with no "
                           "disk at all")
        for image in images:
            attached = (image.get("uuid") or "").strip()
            if not (attached.startswith("{") and attached.endswith("}")):
                fail(problems, f"AttachedDevice Image uuid {attached!r} is not "
                               "in braces; VirtualBox writes it that way")
                continue
            bare = attached[1:-1]
            try:
                parsed = uuid.UUID(bare)
            except ValueError:
                fail(problems, f"attached image uuid {bare!r} is not a uuid")
                continue
            if parsed.version is None or parsed.variant != uuid.RFC_4122:
                fail(problems, f"attached image uuid {bare} is uuid-shaped but "
                               "not a valid uuid; VirtualBox reads it as null "
                               "and then cannot find the image")
            if not any((d.get(f"{{{NS['vbox']}}}uuid") or "") == bare
                       for d in disks):
                fail(problems, f"no Disk carries vbox:uuid=\"{bare}\" (bare, "
                               "no braces) to match the machine's attachment")

        # VirtualBox reads firmware from its own machine section. The
        # elements inside vbox:Machine inherit the document's default
        # namespace rather than the vbox one, and which namespace that is
        # differs between VirtualBox versions, so match on the local name:
        # that is what VirtualBox itself keys on, and an XPath that assumed
        # otherwise found nothing in a file that was in fact correct.
        machine = root.find(".//vbox:Machine", NS)
        vbox_fw = None
        if machine is not None:
            for el in machine.iter():
                if el.tag.rsplit("}", 1)[-1] == "Firmware":
                    vbox_fw = el
                    break
        if vbox_fw is None or vbox_fw.get("type") != "EFI":
            fail(problems, "VirtualBox firmware is not set to EFI; the guest "
                           "would import onto BIOS and never boot")

        # VMware reads it from a vmw:Config entry.
        vmw_ok = any(
            item.get("{%s}key" % NS["vmw"]) == "firmware"
            and item.get("{%s}value" % NS["vmw"]) == "efi"
            for item in root.findall(".//vmw:Config", NS))
        if not vmw_ok:
            fail(problems, "VMware firmware config is not efi; the guest would "
                           "boot on BIOS and show a blank screen")

        # The manifest, if present, must actually describe the archive.
        mf = [m for m in members if m.endswith(".mf")]
        if mf:
            mf_text = tar.extractfile(mf[0]).read().decode("utf-8")
            for name, digest in re.findall(r"SHA256\(([^)]+)\)=\s*([0-9a-f]+)",
                                           mf_text):
                if name not in members:
                    fail(problems, f"manifest lists {name}, absent from the ova")
                    continue
                h = hashlib.sha256()
                fp = tar.extractfile(name)
                for chunk in iter(lambda: fp.read(1 << 20), b""):
                    h.update(chunk)
                if h.hexdigest() != digest:
                    fail(problems, f"manifest hash for {name} does not match")


def check_vmx(path, problems):
    with open(path, encoding="utf-8") as fp:
        text = fp.read()
    if not re.search(r'^firmware\s*=\s*"efi"', text, re.M):
        fail(problems, "vmx does not select efi firmware")
    if not re.search(r'^sata0:0\.fileName\s*=\s*"(.+)"', text, re.M):
        fail(problems, "vmx attaches no disk")
    else:
        disk = re.search(r'^sata0:0\.fileName\s*=\s*"(.+)"', text, re.M).group(1)
        if not os.path.exists(os.path.join(os.path.dirname(path), disk)):
            fail(problems, f"vmx points at {disk}, which is not beside it")


def main():
    art = sys.argv[1] if len(sys.argv) > 1 else "build-gcc-Release/artifacts"
    problems = []

    ova = os.path.join(art, "vibeos.ova")
    vmx = os.path.join(art, "vibeos.vmx")
    for path in (ova, vmx):
        if not os.path.exists(path):
            print(f"[VM-CHECK] missing: {path}")
            return 1

    check_ova(ova, problems)
    check_vmx(vmx, problems)

    if problems:
        for p in problems:
            print(f"[VM-CHECK] {p}")
        print(f"[VM-CHECK] FAIL ({len(problems)} problems)")
        return 1
    print("[VM-CHECK] PASS appliance is well formed and asks for EFI")
    return 0


if __name__ == "__main__":
    sys.exit(main())
