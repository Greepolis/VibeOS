#!/bin/bash
# verify-boot.sh, keeping the serial log of every boot it verifies.
#
# For SABOTAGE_VERIFY. A sabotage run boots once per case and each boot
# overwrites qemu-cli-serial.log, so by the end only the last case's log is left
# - and a case that went red for the wrong reason, or NOT RED, is exactly the
# one whose log has to be read. This copies each one to
# .boot-evidence/sab-<n>.log and prints the path under the gate's reason.
#
# Move old sab-*.log files aside before a run rather than deleting them: they
# are the evidence of the previous one (CLAUDE.md, "never delete the evidence").
#
#   SABOTAGE_VERIFY=scripts/dev/verify-boot-keep.sh \
#       python3 scripts/dev/sabotage.py <source> <cases.txt>
cd "$(dirname "$0")/../.." || exit 1
bash scripts/dev/verify-boot.sh "$@"
rc=$?
mkdir -p .boot-evidence
n=$(ls .boot-evidence/sab-*.log 2>/dev/null | wc -l)
cp qemu-cli-serial.log ".boot-evidence/sab-$((n + 1)).log"
echo "    log=.boot-evidence/sab-$((n + 1)).log"
exit $rc
