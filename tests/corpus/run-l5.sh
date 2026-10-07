# The sockets' programs (docs/abi/ L5 step 4), run by the boot's shell as
# /corpus/progs5.sh after SOCKETS.ELF has run its own checks. Each prints what
# came back; the boot gate reads it. No marker the gate asserts on appears here
# as typed: the page is one the gate made up for this run, the address is what
# the resolver answered.

# wget: BusyBox's HTTP client against the host's server - a connect, a request
# written, a response read to the end. Only the first boot of a run has the
# server (qemu-cli-smoke-linux.py, SKIP_ECHO), as with the echo.
echo "L5P_WGET_SAW $(timeout 15 wget -q -O - http://10.0.2.2:7778/l5 2>/tmp/wget.err) $(cat /tmp/wget.err)"

# DNS through the C library: musl's getaddrinfo reads /etc/resolv.conf and asks
# QEMU's resolver, which asks the host's. The gate fills in a name only when
# the host itself can resolve it; otherwise the line says so and the verdict
# carries dns_absent.
N="@L5_DNS_NAME@"
if [ -n "$N" ]; then
    timeout 15 /EFI/BOOT/SOCKETS.ELF resolve "$N"
else
    echo "L5P_DNS_NOT_ASKED"
fi

# The phase's own checks, a Unix-socket server and client among them, in two
# processes: SOCKETS.ELF prints SOCKETS_OK only if every one held.
/EFI/BOOT/SOCKETS.ELF
echo "L5P_SOCKETS_RC $?"
echo "L5P_DONE_$((5*9))"
