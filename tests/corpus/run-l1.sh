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
    # Not "2> /dev/null": there is no /dev until L2, and the message is part of
    # the answer anyway.
    rmdir a/full
    echo "rmdir of a full directory: $?"
}

w_pipeline() {
    yes | head -n 1000 | wc -l
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
# Leave nothing behind: what stays in /tmp is memory the machine does not get
# back, and the boot gate counts it.
cd /
rm -rf "$W"
echo "C:done: 18"
