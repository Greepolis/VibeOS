# The corpus's file workloads, run for their answers (docs/abi/ L1 step 8).
#
#     busybox sh run-l1.sh
#
# The same script runs in two places: on VibeOS, where the boot stages it as
# /corpus/run.sh and BusyBox's shell runs it in /tmp, and on Linux, where
# scripts/dev/corpus-expect.sh runs it under the same BusyBox and keeps what it
# printed as tests/corpus/l1-expected.txt. The boot gate compares the two line
# for line. So the oracle for every workload here is Linux itself, running the
# same binary - nobody wrote down what the answer should be.
#
# tests/corpus/workloads.txt is what the programs *ask for* (measured under
# strace); this is what they are *expected to answer*. The commands are the
# same ones, shaped so that the answer does not depend on things two machines
# are allowed to differ in: the date, the owner's name, how many blocks a
# directory takes, the order a directory happens to list in. What is left -
# names, contents, types, modes, link counts, sizes, exit codes - has to match.
#
# Every line of a workload's output goes out as "C:<name>: <line>", with its
# exit code last, so the gate can find it in a serial log among everything else
# the kernel prints. A program that is not staged prints "absent": the third-
# party ones (SQLite, Lua) are built by scripts/dev/corpus-build.sh, which a
# plain build does not run.

C=${CORPUS_DIR:-/corpus}
W=${CORPUS_WORK:-/tmp/corpus}
export HOME=/
export TZ=UTC

rm -rf "$W"
mkdir -p "$W" || { echo "C:setup: cannot make $W"; exit 1; }

# The fixture scripts/dev/corpus-measure.sh gives each workload.
fixture() {
    printf 'hello from a file\nsecond line with hello\n' > note.txt
    mkdir -p dir/sub
    printf 'hello\n' > dir/a.txt
    printf 'world\n' > dir/sub/b.txt
    ln -sf a.txt dir/link
}

# run <name> <function>: in a directory of its own, output and exit code kept
# in a file and then printed tagged - so nothing a workload prints goes to the
# terminal, where ls would lay it out in columns and a C library would buffer
# it by lines.
run() {
    mkdir "$W/$1" && cd "$W/$1" || { echo "C:$1: cannot enter its directory"; return; }
    fixture
    ( $2 ) > "$W/$1.out" 2>&1
    echo "rc=$?" >> "$W/$1.out"
    cd "$W"
    sed "s/^/C:$1: /" "$W/$1.out"
}

# perms, name and link target of `ls -l` lines: not the owner, size of a
# directory or date.
ls_shape() {
    awk '$1 != "total" { print $1, $9, $10, $11 }'
}

w_sh_script() {
    for i in 1 2 3; do echo $i > f$i; done
    cat f1 f2 f3 | wc -l
    test -f f1 && echo ok
    x=$(echo sub)
    echo "$x"
    trap "echo bye" EXIT
}

w_ls() {
    ls dir
    ls -la dir | ls_shape
    ls -l dir/a.txt | awk '{ print $1, $2, $5 }'
}

w_cp_mv_rm() {
    cp -a dir dir2 && mv dir2 dir3 && ls dir3 dir3/sub && cat dir3/link dir3/sub/b.txt
    readlink dir3/link
    rm -rf dir3 && ls
}

w_find_grep() {
    find . -name "*.txt" | sort | xargs grep -l hello
    find . -type l
    find . -type d | sort
}

w_grep_r() {
    grep -rn hello dir | sort
}

w_tar() {
    tar cf a.tar dir && mkdir x && tar xf a.tar -C x && ls x/dir
    cat x/dir/sub/b.txt
    readlink x/dir/link
    tar tf a.tar | sort
}

w_text() {
    sort note.txt | sed s/hello/hi/ | awk "{print NF}" | head -n 1 | tail -n 1
    sort -r note.txt | sed s/hello/hi/
    wc -l note.txt
}

w_sed_inplace() {
    sed -i s/hello/bye/ note.txt
    cat note.txt
    ls
}

w_metadata() {
    touch t
    chmod 600 t
    ln -s t l
    readlink l
    stat -c '%a %h %s %F' t
    stat -c '%F' l
    ls -l l | ls_shape
    touch -d '2000-01-01 00:00:00' t
    stat -c '%Y' t
    ln t hard
    stat -c '%h' t
    echo more >> hard
    cat t
    chmod 444 hard
    stat -c '%a' t
}

w_mkdir_rm() {
    mkdir -p a/b/c && rmdir a/b/c && ls a/b && rm -r a && ls
    mkdir a && rmdir a/missing
    echo "rmdir of nothing: $?"
    mkdir -p a/full/x
    # Not "2> /dev/null": the message is part of the answer.
    rmdir a/full
    echo "rmdir of a full directory: $?"
}

w_pipeline() {
    # yes's own complaint is not part of the answer. Three nightlies running
    # this on Linux (2026-10-02 to 04) printed "yes: Broken pipe" - yes got
    # EPIPE instead of dying of SIGPIPE when head left - and read that line as
    # Linux answering differently. Not reproduced here, with SIGPIPE ignored by
    # bash or by Python; the line is dropped because it says nothing about the
    # kernel either way, not because the cause is known.
    yes 2>/dev/null | head -n 1000 | wc -l
    seq 1 100 | sort -rn | head -n 3
}

w_mv_across() {
    # /tmp and the directory this script came from are two filesystems on
    # VibeOS and may be one on Linux; within the work directory a rename is a
    # rename everywhere.
    echo data > one
    mv one two && cat two
    mkdir d1 && mv two d1/three && ls d1
    mv d1 d2 && cat d2/three
}

w_sqlite_memory() {
    [ -x "$C/sqlite3" ] || { echo absent; return 0; }
    "$C/sqlite3" :memory: 'create table t(a, b); insert into t values (1, "x"), (2, "y"); select sum(a), group_concat(b) from t;'
}

w_sqlite_file() {
    [ -x "$C/sqlite3" ] || { echo absent; return 0; }
    "$C/sqlite3" db.sqlite 'create table t(a integer primary key, b text); insert into t(b) values ("one"), ("two"), ("three"); create index tb on t(b); select count(*) from t; vacuum; pragma integrity_check;'
    # And again, by another process: what the first one wrote is on the file.
    "$C/sqlite3" db.sqlite 'insert into t(b) values ("four"); select group_concat(b) from t order by a; pragma integrity_check;'
    ls
}

w_lua_script() {
    [ -x "$C/lua" ] || { echo absent; return 0; }
    "$C/lua" -e 'local t = {} for i = 1, 1000 do t[i] = i * i end print(#t) local f = io.open("x.txt", "w") f:write("hi") f:close() print(io.open("x.txt"):read("a")) print(os.time() > 0, os.clock() >= 0) os.remove("x.txt") print(os.getenv("HOME") ~= nil)'
}

# ---- docs/abi/ L3 step 6: the programs memory was for --------------------------------
#
# The same two programs built against glibc and linked dynamically: the loader
# is glibc's own, which reserves a range and lays the library's segments over
# it with MAP_FIXED. And SQLite in write-ahead-log mode with memory-mapped I/O:
# the WAL's index is a file every connection maps shared - without a shared
# mapping of a file the journal mode cannot be entered at all - and the
# database itself is read through a mapping when mmap_size says so.

w_glibc_sqlite() {
    [ -x "$C/sqlite3-glibc" ] || { echo absent; return 0; }
    "$C/sqlite3-glibc" g.sqlite 'create table t(a integer primary key, b text); insert into t(b) values ("one"), ("two"), ("three"); create index tb on t(b); select count(*), group_concat(b) from t; vacuum; pragma integrity_check;'
    "$C/sqlite3-glibc" g.sqlite 'select sum(a), max(b), round(avg(a), 2), upper(min(b)) from t;'
}

w_glibc_lua() {
    [ -x "$C/lua-glibc" ] || { echo absent; return 0; }
    "$C/lua-glibc" -e 'local t = {} for i = 1, 1000 do t[i] = i * i end print(#t, t[1000]) local f = io.open("y.txt", "w") f:write("hi") f:close() print(io.open("y.txt"):read("a")) print(string.format("%.3f %5d %s", math.pi, 42, "x")) print(os.time() > 0) os.remove("y.txt")'
}

w_sqlite_wal_mmap() {
    [ -x "$C/sqlite3" ] || { echo absent; return 0; }
    "$C/sqlite3" w.sqlite 'pragma journal_mode=wal; pragma mmap_size=1048576; create table t(a integer primary key, b text); with recursive n(i) as (select 1 union all select i + 1 from n where i < 400) insert into t(b) select "row " || i from n; select count(*), sum(a), max(b) from t;'
    # A second connection reads through the log and the mapping, and puts the
    # log back into the database.
    "$C/sqlite3" w.sqlite 'pragma mmap_size=1048576; select count(*) from t where b like "row 3%"; update t set b = "changed" where a = 200; select b from t where a between 199 and 201; pragma wal_checkpoint(truncate); pragma integrity_check; pragma journal_mode;'
    ls
}

# ---- docs/abi/ L2 step 7: the programs processes and signals were for ------------
#
# A shell's own process work: traps, signals to itself and to its children,
# timeout, time, ps, job control's wait, limits and nice, and what /proc and
# /dev say. What two machines may differ in is left out as it is above - pids,
# times, owners, the name a NOEXEC applet runs under (BusyBox runs some applets
# in a forked shell without an exec, so their comm and cmdline are the shell's).
# What is left is states, relationships, codes and the messages a shell prints
# for a child a signal ended: "Terminated" is the wait status read correctly.
#
# A trap that the shell must take from a signal it sends itself runs in a shell
# of its own: in run()'s subshell $$ is still the script's pid, and the signal
# would go to the script.

w_trap() {
    sh -c 'trap "echo caught USR1" USR1; kill -USR1 $$; echo after it; trap'
    sh -c 'trap "echo caught TERM; exit 3" TERM; kill -TERM $$; echo not reached'
    echo "a trap that exits: $?"
    sh -c 'trap "" INT; kill -INT $$; echo INT ignored'
    sh -c 'trap "echo on exit" EXIT; exit 4'
    echo "the exit trap keeps the code: $?"
    sh -c 'kill -TERM $$; echo not reached'
    echo "no trap: $?"
    sh -c 'trap "echo hup" HUP; kill -HUP $$; trap - HUP; trap "" HUP; kill -HUP $$; echo HUP ignored now'
    kill -l 15
    kill -l TERM
}

w_children() {
    sleep 5 &
    p=$!
    kill -0 $p && echo "the child is there"
    kill -TERM $p
    wait $p
    echo "ended by TERM: $?"
    kill -0 $p 2> /dev/null
    echo "and gone: $?"
    sleep 5 &
    p=$!
    kill -KILL $p
    wait $p
    echo "ended by KILL: $?"
    (exit 9) &
    wait $!
    echo "its own code: $?"
    sleep 5 &
    p=$!
    (sleep 0.2; kill $p) &
    wait $p
    echo "woken by the kill: $?"
    wait
    sleep 1 &
    sleep 1 &
    jobs | wc -l
    wait
    echo "waited for all: $?"
}

w_timeout() {
    timeout 1 sleep 5
    echo "timed out: $?"
    timeout -s KILL 1 sleep 5
    echo "killed: $?"
    timeout 5 true
    echo "in time: $?"
    timeout 5 sh -c 'exit 7'
    echo "the program's code: $?"
}

w_time() {
    time -p sleep 0.3 2> t.txt
    awk '$1 == "real" { print ($2 >= 0.3 ? "slept for long enough" : "returned early: " $2) }' t.txt
    awk '{ print $1, ($2 ~ /^[0-9]+\.[0-9][0-9]$/ ? "a number" : $2) }' t.txt
    time -p sh -c 'exit 5' 2> t.txt
    echo "time passes the code on: $?"
    # CPU time, which time reads from the rusage wait4 hands back. Bounded by
    # the clock, not by a count: ten thousand iterations took under five
    # milliseconds on a CI runner, time -p printed 0.00 twice, and the nightly
    # said Linux had changed its answer (2026-10-08). Two seconds of work is
    # more than a hundredth on any machine, the guest's TCG included.
    time -p sh -c 's=$(date +%s); while [ $(( $(date +%s) - s )) -lt 2 ]; do :; done' 2> t.txt
    awk '$1 == "user" { u = $2 } $1 == "sys" { s = $2 } END { print (u + s > 0 ? "it ran for a while" : "no CPU time: " u " " s) }' t.txt
}

w_ps() {
    sleep 3 &
    p=$!
    # Until it is in its sleep: a child just forked is running, not waiting.
    n=0
    while [ "$(ps -o pid,stat | awk -v p=$p '$1 == p { print $2 }')" != S ] && [ $n -lt 50 ]; do
        sleep 0.1
        n=$((n + 1))
    done
    ps -o pid,ppid,pgid,stat > ps.txt
    pp=$(awk -v p=$p '$1 == p { print $2 }' ps.txt)
    awk -v p=$p '$1 == p { print "the child:", $4 }' ps.txt
    awk -v p=$p -v pp=$pp '$1 == p { g = $3 } $1 == pp { pg = $3; print "its parent:", $4 } END { print (g == pg ? "one group" : "two groups") }' ps.txt
    head -n 1 ps.txt
    kill $p
    wait $p
    ps -o pid | awk -v p=$p '$1 == p' | wc -l
}

w_proc() {
    awk '{ print NF, $3 }' /proc/self/stat
    awk '{ print NF }' /proc/self/statm
    awk '$1 == "State:" { print $1, $2 }' /proc/self/status
    awk '$6 == "[stack]" { print "a stack" }' /proc/self/maps
    readlink /proc/self/cwd | sed "s|^$W/||"
    test -d /proc/$$ && echo "the script has a directory"
    test -L /proc/self && echo "self is a link"
    awk '{ print ($1 > 0 ? "up" : "not up"), NF }' /proc/uptime
    awk '{ print NF }' /proc/loadavg
    # Not how many descriptors there are: that is whatever the caller of the
    # script left open. Standard error is run()'s output file.
    readlink /proc/self/fd/2 | sed "s|^$W/||"
}

w_dev() {
    ls -l /dev/null /dev/zero /dev/full /dev/random /dev/urandom /dev/tty | awk '{ print $1, $5, $6, $NF }'
    ls -l /dev/stdin /dev/stdout /dev/stderr /dev/fd | awk '{ print $1, $(NF - 2), $(NF - 1), $NF }'
    echo gone > /dev/null
    echo "to null: $?"
    wc -c < /dev/null
    head -c 3 /dev/zero | od -An -tx1
    head -c 4096 /dev/urandom | wc -c
    dd if=/dev/random bs=16 count=2 2> /dev/null | wc -c
    a=$(head -c 16 /dev/urandom | od -An -tx1)
    b=$(head -c 16 /dev/urandom | od -An -tx1)
    [ "$a" != "$b" ] && echo "two reads differ"
    echo x > /dev/full
    echo "to full: $?"
}

w_limits() {
    ulimit -n 64
    ulimit -n
    ulimit -S -n 32
    ulimit -n
    ulimit -H -n
    # The shell that waits says "File size limit exceeded", and Linux adds
    # "(core dumped)" when its core_pattern is a pipe - a host's setting, which
    # ignores RLIMIT_CORE - so that shell's stderr is not part of the answer.
    sh -c 'ulimit -f 1; head -c 4096 /dev/zero > big; echo "past the file size: $?"' 2> /dev/null
    wc -c < big
    # Not nice: this BusyBox has no such applet, and Linux answered from
    # coreutils' - renice is BusyBox's, and a child keeps what its parent set.
    awk '{ print $19 }' /proc/self/stat
    sh -c 'renice -n 5 -p $$ > /dev/null; awk "{ print \$19 }" /proc/self/stat'
}

run bb-sh-script w_sh_script
run bb-ls w_ls
run bb-cp-mv-rm w_cp_mv_rm
run bb-find-grep w_find_grep
run bb-grep-r w_grep_r
run bb-tar w_tar
run bb-text w_text
run bb-sed-inplace w_sed_inplace
run bb-metadata w_metadata
run bb-mkdir-rm w_mkdir_rm
run bb-pipeline w_pipeline
run bb-mv w_mv_across
run sqlite-memory w_sqlite_memory
run sqlite-file w_sqlite_file
run lua-script w_lua_script
run glibc-sqlite w_glibc_sqlite
run glibc-lua w_glibc_lua
run sqlite-wal-mmap w_sqlite_wal_mmap
run bb-trap w_trap
run bb-kill-wait w_children
run bb-timeout w_timeout
run bb-time w_time
run bb-ps w_ps
run bb-proc w_proc
run bb-dev w_dev
run bb-limits w_limits
# Leave nothing behind: what stays in /tmp is memory the machine does not get
# back, and the boot gate counts it.
cd /
rm -rf "$W"
echo "C:done: 26"
