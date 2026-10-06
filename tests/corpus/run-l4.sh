# The event loops' programs (docs/abi/ L4 step 7), run by the boot's shell as
# /corpus/progs.sh. Each prints a line that says what it saw; the boot gate
# reads them, and drives the two that need someone on the host - it fetches the
# page httpd serves and connects to the nc that listens - through QEMU's
# forwarded ports. No marker the gate asserts on appears in a command here as
# typed: each is built by the shell's arithmetic or comes back from the other
# end.

# tail -f: follows a file that grows - it sleeps and looks again.
: > /tmp/tf.log
tail -f /tmp/tf.log > /tmp/tf.out &
T=$!
sleep 1
echo "L4P_TAILF_$((40+2))" >> /tmp/tf.log
sleep 2
kill $T
wait $T 2>/dev/null
echo "L4P_TAILF_SAW $(cat /tmp/tf.out)"

# httpd: serves /tmp/www on 8080, a child per connection. In the foreground of
# a background job rather than as a daemon, so that it can be stopped: the
# machine leaves userland only once every user task has ended, and a daemon
# nobody can name would hold it there. The gate fetches the page from the
# host when it sees READY.
mkdir -p /tmp/www
echo "L4P_BODY_$((6*7))" > /tmp/www/index.html
httpd -f -p 8080 -h /tmp/www 2> /tmp/httpd.err &
H=$!
sleep 1
echo "L4P_HTTPD_$((1+1))_READY"

# nc out: to the host's echo server, which answers once and closes. Only the
# first boot of a run has one (qemu-cli-smoke-linux.py, SKIP_ECHO).
echo "L4P_NCOUT_$((3*3))" | timeout 10 nc 10.0.2.2 7777 > /tmp/nco.out 2>/dev/null
echo "L4P_NCOUT_SAW $(cat /tmp/nco.out)"

# nc in: listens on 8081 for what the gate sends; its standard input is empty,
# so it shuts its side at once and prints what arrives until the host closes.
timeout 20 nc -l -p 8081 > /tmp/nci.out 2> /tmp/nci.err &
N=$!
sleep 1
echo "L4P_NCIN_$((2+2))_READY"
wait $N
echo "L4P_NCIN_RC $? $(cat /tmp/nci.err)"
echo "L4P_NCIN_SAW $(cat /tmp/nci.out)"

# The page has been fetched by now, or never will be.
sleep 1
kill $H
wait $H 2>/dev/null
echo "L4P_HTTPD_ERR $(cat /tmp/httpd.err)"
echo "L4P_DONE_$((7*6))"
