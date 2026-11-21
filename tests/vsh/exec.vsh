# vsh running, the same script on Linux and in the guest (docs/shell/s4.md).
# Every answer is a line starting V: and tests/vsh/exec-expected.txt says what
# each must be. It was written by hand from the grammar, not from a run.
# The first argument is the path of vsh itself.

set vsh = ${args[0]}
set work = /tmp/vsh-exec
try rm -rf $work
mkdir -p $work
cd $work
echo "V:pwd $(pwd)"
echo "V:cd %CD%"

# no word splitting; only a list is several words
set a = "two words"
for w in $a { echo "V:word $w" }
set l = (a "b c" d)
for w in $l { echo "V:list $w" }
echo "V:idx ${l[1]}"
echo "V:joined $l"
set l = ($l e)
echo "V:grown ${l[3]}"

# conditions
set n = 10
if $n >= 10 && $a != "root" { echo V:if-big } else { echo V:if-small }
if $n < 9 { echo V:no } else if $a ~ two* { echo V:elseif } else { echo V:no }
if $a ~~ TWO* { echo V:nocase }
if (grep -q nothing /dev/null) { echo V:no } else { echo V:cmd-false }
if ! (false) { echo V:not }

# loops
set i = 3
while $i > 0 {
    set i = $(( $i - 1 ))
    if $i == 1 { continue }
    echo "V:while $i"
}
for x in p q r {
    if $x == q { break }
    echo "V:for $x"
}

# functions: local by default, global on request
set outer = kept
func f who {
    set outer = changed
    global made = "by f"
    echo "V:f $who ${args[1]}"
    return 3
}
try f me extra
echo "V:status $?"
echo "V:outer $outer"
echo "V:global $made"
func fact k {
    if $k <= 1 { echo 1; return 0 }
    set rest = $(fact $(( $k - 1 )))
    echo $(( $k * $rest ))
}
echo "V:fact $(fact 5)"

# pipes and redirections
echo one > f.txt
echo two >> f.txt
echo three >> f.txt
echo "V:lines $(cat f.txt | wc -l)"
echo "V:grep $(grep th < f.txt | sort | cat)"
echo "V:stderr $(cat /no/such/file 2>&1 | wc -l)"
cat /no/such/file > e.txt 2>&1 || echo "V:or $?"
echo "V:captured $(wc -l < e.txt)"
echo "V:after-redirect"

# files by pattern, sorted; a quoted one is not a pattern
mkdir sub
echo x > sub/b.c
echo x > sub/a.c
echo x > sub/.hidden.c
for p in sub/*.c { echo "V:glob $p" }
echo "V:quoted" "sub/*.c"
for p in sub/*.none { echo "V:nomatch $p" }

# the environment
X=1 sh -c 'echo "V:env-one $X"'
sh -c 'echo "V:env-gone [$X]"'
export Y = exported
sh -c 'echo "V:env-export $Y"'

# aliases, and case
alias say = echo V:alias
say hello
SAY again
ECHO V:builtin-case
echo "V:$(type cd)"
unalias say

# literals
echo 'V:single $a'
echo V:back\ slash C:\Users\vibe
echo V:percent +%d%m%Y

# the first failure stops a script
set vs = $vsh
echo "V:stops $($vs -c 'echo a; false; echo b' ; true)"
try $vs -c 'false'
echo "V:rc $?"
echo "V:off $($vs -c 'set -e off; false; echo b')"
echo "V:try $($vs -c 'try false; echo b')"
echo "V:andor $($vs -c 'false && echo no; echo b')"
echo "V:sigpipe $($vs -c 'yes | head -n 1; echo b' | wc -l)"
try $vs -c 'x=1' 2> parse.txt
echo "V:bare $?"
echo "V:hint $(grep -c 'set x = 1' parse.txt)"

cd /
rm -rf $work
echo V:done
