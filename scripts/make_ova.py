#!/usr/bin/env python3
"""Package a built VibeOS disk as a VM you can import by double-clicking.

    python3 scripts/make_ova.py <esp.img> <out-dir> [--name VibeOS]

Produces `vibeos.ova` (VirtualBox: File -> Import Appliance; VMware
Workstation/Player: File -> Open) and `vibeos.vmx` alongside a plain VMDK for
people who would rather wire the VM up themselves.

A .vdi or .vmdk on its own is a *disk*, not a machine: importing one still
leaves you creating a VM, choosing a chipset, and - the step everybody misses -
turning on EFI. VibeOS boots through UEFI and nothing else, so a VM left on the
default BIOS firmware does not fail loudly, it sits at a blank screen. Carrying
the firmware setting inside the appliance is the whole reason this file exists.

The OVF below therefore says "EFI" twice, once in each vendor's dialect:
VirtualBox reads `vbox:Machine/@firmwareType`, VMware reads a `vmw:Config` with
key `firmware`. Each ignores the other's, so both can be present and there is
no third file to keep in step.
"""
import argparse
import hashlib
import os
import subprocess
import sys
import tarfile

OVF_TEMPLATE = """<?xml version="1.0" encoding="UTF-8"?>
<Envelope ovf:version="1.0" xml:lang="en-US"
    xmlns="http://schemas.dmtf.org/ovf/envelope/1"
    xmlns:ovf="http://schemas.dmtf.org/ovf/envelope/1"
    xmlns:rasd="http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_ResourceAllocationSettingData"
    xmlns:vssd="http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_VirtualSystemSettingData"
    xmlns:vbox="http://www.virtualbox.org/ovf/machine"
    xmlns:vmw="http://www.vmware.com/schema/ovf">
  <References>
    <File ovf:id="file1" ovf:href="{disk_name}" ovf:size="{disk_size}"/>
  </References>
  <DiskSection>
    <Info>Virtual disk information</Info>
    <Disk ovf:capacity="{capacity}" ovf:diskId="vmdisk1" ovf:fileRef="file1"
          ovf:format="http://www.vmware.com/interfaces/specifications/vmdk.html#streamOptimized"/>
  </DiskSection>
  <NetworkSection>
    <Info>The list of logical networks</Info>
    <Network ovf:name="NAT">
      <Description>Logical network used by this appliance.</Description>
    </Network>
  </NetworkSection>
  <VirtualSystem ovf:id="{name}">
    <Info>A virtual machine</Info>
    <Name>{name}</Name>
    <OperatingSystemSection ovf:id="1" vmw:osType="otherGuest64">
      <Info>The kind of installed guest operating system</Info>
      <Description>Other (64-bit)</Description>
    </OperatingSystemSection>
    <VirtualHardwareSection>
      <Info>Virtual hardware requirements for a virtual machine</Info>
      <System>
        <vssd:ElementName>Virtual Hardware Family</vssd:ElementName>
        <vssd:InstanceID>0</vssd:InstanceID>
        <vssd:VirtualSystemIdentifier>{name}</vssd:VirtualSystemIdentifier>
        <vssd:VirtualSystemType>virtualbox-2.2</vssd:VirtualSystemType>
      </System>
      <Item>
        <rasd:Caption>{cpus} virtual CPU</rasd:Caption>
        <rasd:Description>Number of virtual CPUs</rasd:Description>
        <rasd:ElementName>{cpus} virtual CPU</rasd:ElementName>
        <rasd:InstanceID>1</rasd:InstanceID>
        <rasd:ResourceType>3</rasd:ResourceType>
        <rasd:VirtualQuantity>{cpus}</rasd:VirtualQuantity>
      </Item>
      <Item>
        <rasd:AllocationUnits>MegaBytes</rasd:AllocationUnits>
        <rasd:Caption>{memory} MB of memory</rasd:Caption>
        <rasd:Description>Memory Size</rasd:Description>
        <rasd:ElementName>{memory} MB of memory</rasd:ElementName>
        <rasd:InstanceID>2</rasd:InstanceID>
        <rasd:ResourceType>4</rasd:ResourceType>
        <rasd:VirtualQuantity>{memory}</rasd:VirtualQuantity>
      </Item>
      <Item>
        <rasd:Address>0</rasd:Address>
        <rasd:Caption>sataController0</rasd:Caption>
        <rasd:Description>SATA Controller</rasd:Description>
        <rasd:ElementName>sataController0</rasd:ElementName>
        <rasd:InstanceID>3</rasd:InstanceID>
        <rasd:ResourceSubType>AHCI</rasd:ResourceSubType>
        <rasd:ResourceType>20</rasd:ResourceType>
      </Item>
      <Item>
        <rasd:AddressOnParent>0</rasd:AddressOnParent>
        <rasd:Caption>disk1</rasd:Caption>
        <rasd:Description>Disk Image</rasd:Description>
        <rasd:ElementName>disk1</rasd:ElementName>
        <rasd:HostResource>/disk/vmdisk1</rasd:HostResource>
        <rasd:InstanceID>4</rasd:InstanceID>
        <rasd:Parent>3</rasd:Parent>
        <rasd:ResourceType>17</rasd:ResourceType>
      </Item>
      <Item>
        <rasd:AutomaticAllocation>true</rasd:AutomaticAllocation>
        <rasd:Caption>Ethernet adapter on 'NAT'</rasd:Caption>
        <rasd:Connection>NAT</rasd:Connection>
        <rasd:ElementName>Ethernet adapter on 'NAT'</rasd:ElementName>
        <rasd:InstanceID>5</rasd:InstanceID>
        <rasd:ResourceSubType>virtio</rasd:ResourceSubType>
        <rasd:ResourceType>10</rasd:ResourceType>
      </Item>
      <!-- VMware reads the firmware setting from here. Without it the guest
           gets BIOS, and a UEFI-only system on BIOS shows a blank screen
           rather than an error. -->
      <vmw:Config ovf:required="false" vmw:key="firmware" vmw:value="efi"/>
    </VirtualHardwareSection>
    <!-- VirtualBox ignores the section above for firmware and reads this one.
         Both are present on purpose; neither hypervisor minds the other's. -->
    <vbox:Machine ovf:required="false" version="1.16-linux"
                  uuid="{machine_uuid}" name="{name}" OSType="Other_64">
      <ovf:Info>VirtualBox machine configuration in VirtualBox format</ovf:Info>
      <Hardware>
        <CPU count="{cpus}"/>
        <Memory RAMSize="{memory}"/>
        <Firmware type="EFI"/>
        <Boot>
          <Order position="1" device="HardDisk"/>
        </Boot>
      </Hardware>
    </vbox:Machine>
  </VirtualSystem>
</Envelope>
"""

VMX_TEMPLATE = """.encoding = "UTF-8"
config.version = "8"
virtualHW.version = "14"
displayName = "{name}"
guestOS = "other-64"
# UEFI, not BIOS. VibeOS has no legacy boot path, and on BIOS this VM shows a
# blank screen rather than saying anything.
firmware = "efi"
numvcpus = "{cpus}"
memsize = "{memory}"
sata0.present = "TRUE"
sata0:0.present = "TRUE"
sata0:0.fileName = "{disk_name}"
sata0:0.deviceType = "disk"
ethernet0.present = "TRUE"
ethernet0.connectionType = "nat"
ethernet0.virtualDev = "vmxnet3"
serial0.present = "TRUE"
serial0.fileType = "file"
serial0.fileName = "vibeos-serial.log"
# The serial line is where this system says what it is doing; without it a
# failed boot is a blank window and nothing else.
"""


def run(cmd):
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} failed: {proc.stderr.strip()}")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fp:
        for chunk in iter(lambda: fp.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def build(esp, out_dir, name, cpus, memory):
    os.makedirs(out_dir, exist_ok=True)
    disk_name = f"{name.lower()}-disk1.vmdk"
    disk_path = os.path.join(out_dir, disk_name)

    # streamOptimized is the subformat OVF expects; a plain vmdk imports in
    # VirtualBox but VMware's own tooling rejects the appliance.
    run(["qemu-img", "convert", "-f", "raw", "-O", "vmdk",
         "-o", "subformat=streamOptimized", esp, disk_path])

    capacity = os.path.getsize(esp)
    # VirtualBox refuses an appliance whose vbox:Machine has no uuid - the
    # import fails with "Required Machine/@uuid or @name attributes is
    # missing", which names the attribute it wants and is easy to read as
    # being about the name. Derived from the machine name so that rebuilding
    # the same appliance produces the same identity rather than a new machine
    # every time.
    machine_uuid = "{%s-%s-%s-%s-%s}" % (
        hashlib.sha256(name.encode()).hexdigest()[:8],
        hashlib.sha256(name.encode()).hexdigest()[8:12],
        hashlib.sha256(name.encode()).hexdigest()[12:16],
        hashlib.sha256(name.encode()).hexdigest()[16:20],
        hashlib.sha256(name.encode()).hexdigest()[20:32])

    ovf = OVF_TEMPLATE.format(disk_name=disk_name,
                              machine_uuid=machine_uuid,
                              disk_size=os.path.getsize(disk_path),
                              capacity=capacity, name=name,
                              cpus=cpus, memory=memory)
    ovf_name = f"{name.lower()}.ovf"
    ovf_path = os.path.join(out_dir, ovf_name)
    with open(ovf_path, "w", encoding="utf-8", newline="\n") as fp:
        fp.write(ovf)

    mf_name = f"{name.lower()}.mf"
    mf_path = os.path.join(out_dir, mf_name)
    with open(mf_path, "w", encoding="utf-8", newline="\n") as fp:
        fp.write(f"SHA256({ovf_name})= {sha256(ovf_path)}\n")
        fp.write(f"SHA256({disk_name})= {sha256(disk_path)}\n")

    # Order matters: the descriptor must come first in the archive, because an
    # importer reads it to find out what the rest of the tar contains.
    ova_path = os.path.join(out_dir, f"{name.lower()}.ova")
    with tarfile.open(ova_path, "w", format=tarfile.USTAR_FORMAT) as tar:
        for member, path in ((ovf_name, ovf_path), (mf_name, mf_path),
                             (disk_name, disk_path)):
            tar.add(path, arcname=member)

    vmx_path = os.path.join(out_dir, f"{name.lower()}.vmx")
    with open(vmx_path, "w", encoding="utf-8", newline="\n") as fp:
        fp.write(VMX_TEMPLATE.format(name=name, cpus=cpus, memory=memory,
                                     disk_name=disk_name))

    return ova_path, vmx_path, disk_path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("esp")
    ap.add_argument("out_dir")
    ap.add_argument("--name", default="VibeOS")
    ap.add_argument("--cpus", type=int, default=4)
    ap.add_argument("--memory", type=int, default=1024)
    args = ap.parse_args()

    if not os.path.exists(args.esp):
        print(f"[OVA] disk image not found: {args.esp}", file=sys.stderr)
        return 1
    ova, vmx, disk = build(args.esp, args.out_dir, args.name,
                           args.cpus, args.memory)
    for path in (ova, vmx, disk):
        print(f"[OVA] {path} ({os.path.getsize(path)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
