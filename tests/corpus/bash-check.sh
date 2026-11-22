#!/usr/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# What bash asks of the kernel and of itself (docs/shell/s5.md step 2). Every
# line starts B: and tests/corpus/bash-expected.txt is what this same bash printed
# on Linux; the boot gate compares the two, in order and complete.
set -u
LC_ALL=C
work=/tmp/bash-check.$$
mkdir -p "$work" || exit 1
cd "$work" || exit 1
say() { printf 'B:%s\n' "$*"; }

# ---- the language
a=(uno "due tre" quattro)
say "array ${#a[@]} ${a[1]} ${a[*]:1:2}"
declare -A m=([x]=1 [y]=2)
say "assoc ${m[y]} $(printf '%s\n' "${!m[@]}" | sort | tr '\n' ,)"
s="Hello, World"
say "param ${s,,} ${s^^} ${s/World/bash} ${s#*, } ${s%,*} ${#s} ${s:7:3}"
say "default ${nope:-d} ${s:+set}"
say "arith $(( (7 + 3) * 2 ** 3 % 7 )) $(( 0x10 + 010 ))"
[[ abc123 =~ ^([a-z]+)([0-9]+)$ ]] && say "regex ${BASH_REMATCH[1]} ${BASH_REMATCH[2]}"
[[ file.txt == *.txt ]] && say "pattern ok"
say "brace" {a,b}{1,2} {1..3}
f() { local v=inner; g=global; say "func $1 $v $#"; return 7; }
v=outer; f arg x; say "after $? $v $g"
case "foo.c" in *.h) say no ;; *.c) say "case c" ;; esac
for ((i = 0; i < 3; i++)); do printf -v o '%s%d' "${o:-}" "$i"; done; say "for $o"
n=3; while ((n > 0)); do ((n--)); [ $n -eq 1 ] && break; done; say "while $n"
say "printf $(printf '%05.2f|%x|%-4s|%q' 3.14159 255 ab 'a b')"

# ---- processes, pipes and files
say "subst $(echo inner $(echo nested))"
echo one two | { read -r x y; say "read $y $x"; }
say "here $(cat <<EOF
line $((1 + 1))
EOF
)"
say "herestr $(tr a-z A-Z <<< shout)"
false | true | (exit 3); say "pipestatus ${PIPESTATUS[*]}"
(cd /; say "subshell $PWD"); say "back $([ "$PWD" = "$work" ] && echo same)"
echo first > f; echo second >> f; say "file $(wc -l < f) $(tail -n 1 f)"
exec 3< f; read -r l <&3; exec 3<&-; say "fd $l"
cat nothing-here > out 2>&1; say "stderr rc=$? $([ -s out ] && echo captured)"
mkdir d; touch d/b.c d/a.c d/x.h; say "glob" d/*.c
shopt -s nullglob; say "nullglob" d/*.z; shopt -u nullglob
say "process-subst $(cat <(echo from-fd))"
mapfile -t L < <(printf 'p\nq\n'); say "mapfile ${#L[@]} ${L[1]}"
coproc C { read -r w; echo "got-$w"; }
echo ping >&"${C[1]}"; read -r r <&"${C[0]}"; wait "$C_PID"; say "coproc $r"

# ---- signals and jobs
trap 'say "trap usr1"' USR1; kill -USR1 $$; trap - USR1
sleep 30 & p=$!; kill -TERM $p; wait $p; say "wait $?"
(exit 42) & wait $!; say "bgstatus $?"
t() { trap 'say "exit trap"' EXIT; true; }; (t)
timeout_out=$(trap 'echo caught' TERM; kill -TERM $BASHPID; sleep 0); say "self $timeout_out"

# ---- a script run by the kernel
printf '#!/usr/bin/bash\necho "shebang $1"\n' > s.sh; chmod +x s.sh
say "$(./s.sh arg)"
printf 'echo "no-shebang ran"\n' > plain; chmod +x plain
say "$(./plain)"

cd /
rm -rf "$work"
say "done"
