#!/usr/bin/env python3
"""Do the security checks still have the number of call sites they declare?

These are choke points *because* they are called from one place per path and
defined once. Every security-relevant defect review has found in this project
was in `arch_hw.c`, and that is a consequence of its size: when everything lives
in one scope with no boundaries, a check that should apply everywhere is easy to
forget in one place and nothing notices.

The core refactor (docs/core/) moves four thousand lines of syscall handlers out
of that file. That is exactly the operation which turns one choke point into
two, and the second one is always the one that forgets a case. This check exists
so the move is visible while it is happening rather than afterwards.

## Why the count is asserted in both directions

The obvious failure is a *second* site: somebody copies a handler, copies its
guard, and the two drift. The count going up catches that.

The more likely one here is the count going *down*. `hw_user_range_ok` has 44
sites because every syscall taking a user pointer validates through it, and a
syscall that quietly stops validating is a hole, not a tidy-up. Nothing else in
the tree would notice that.

So a declared number that no longer matches is a failure either way. It is not a
prohibition - adding a syscall legitimately moves the count, and the fix is to
edit the number here, in the change that earns it. That is the same rule this
project already applies to the mm-layering allowance and the coverage baselines:
raise a limit as a decision, not as a reflex.

## What this cannot judge

It counts textual occurrences, not calls. A name inside a comment counts, and a
call reached through a function pointer does not. Both are stated rather than
worked around: a tool that guessed here would produce the confidently wrong
answer this project distrusts, and every one of these guards is called by name
today.

Usage: check-chokepoints.py [--list]
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# include/ since A2: the Linux handlers reach these through vibeos/ksvc.h, and a
# declaration there is as much a site as one in arch_hw_internal.h was.
SCOPE = ("kernel", "include")

# name -> (occurrences, why this one is a choke point)
#
# The counts include the definition and any forward declaration, because
# separating those from calls needs a parser and the number's job is to move
# when something changes, not to be a call count.
CHOKEPOINTS = {
    "linux_signal_permitted": (
        5,   # renamed from hw_signal_permitted with the handlers (A2)
        "who may signal whom. Four callers and a definition; a fifth caller "
        "that forgot the check is a process signalling one it does not own."),
    "hw_user_range_ok": (
        5,   # definition + two declarations + the one call, in ks_user_ok (A2; was linux_user_ok,
             # C4 stage 2b) + a comment in include/vibeos/vmspace.h, in scope since A2
        "the low-level range check has ONE call site, ks_user_ok in "
        "kernel/arch/x86_64/ksvc.c, which only linux_user_ok calls. It was 46, "
        "spread over every handler; that is the criterion C4 was written to "
        "reach. A second call is a handler that decided for itself what a valid "
        "pointer is."),
    "ks_user_ok": (
        3,   # definition, its declaration in vibeos/ksvc.h, and linux_user_ok's call
        "the same door, one level up: the kernel service a personality judges a "
        "user range with. One caller per personality - Linux's is linux_user_ok. "
        "A handler calling it directly is a handler that skipped the dispatcher's "
        "chokepoint."),
    "linux_user_ok": (
        22,   # 20 -> 22 with M-082: the first byte of a path is judged before it is read, in the two places that read it. Before: 18 -> 20 in L1 step 7. ioctl: whether its argument is a pointer,
              # and how long, depends on the request (one helper, linux_ioctl_arg,
              # which every request goes through). And the gathered writev,
              # which checks each element's buffer as the element-by-element
              # path does - an address read out of user memory a moment ago.
              # 17 -> 18 in L1 step 6: fcntl's record locks. Whether its third
              # argument is a pointer depends on the command, and six commands
              # say so - more than a row has descriptors for.
              # 16 -> 17 in L1 step 3: preadv and pwritev check each iovec
              # element's own buffer, as readv and writev do - an address read
              # out of user memory a moment ago, which no row can declare.
              # 15 -> 16 in L1: readlinkat copies a link's contents, whose length
              # only the filesystem knows - the same reason as getcwd's.
              # 14 -> 15 in A4: getcwd writes as many bytes as the working directory
              # is long, which only the kernel knows, so its row cannot declare the
              # range and the handler asks. Before that: definition, a declaration
              # (vibeos/linux_exports.h), and 12 that cannot be descriptors. 15 -> 14 in A2: the pointer engine moved to kernel/abi/abi.c
              # to serve every personality, and is handed linux_user_ok as a function
              # pointer - a site this count cannot see, stated here instead.
        "who asks the dispatcher to judge a user pointer. **This is where the old "
        "'a syscall that stopped checking' alarm lives now.** The pointer arguments of "
        "every syscall are declared in its row (PTRS) and checked by the engine in "
        "dispatch.c; what is left here is what a descriptor cannot say: an iovec "
        "element's own base and readlink's length (read out of user or kernel data "
        "only just fetched), clone's stack and tid words and wait's status word "
        "(stores that are skipped silently, not refused), futex (EINVAL, not "
        "EFAULT), the signal frame and its return, and the kernel's own reads (a "
        "user string, the crash dump). A number going down is a check that vanished; "
        "going up is a handler deciding for itself again."),
    "hw_user_range_why": (
        5,   # 6 -> 5 in A2: execve's two argv refusals ask ks_user_range_why now
        "the same check with a reason attached. A refusal that names a "
        "mechanism instead of a situation cost a session once."),
    "ks_user_range_why": (
        4,   # definition, declaration, execve's two argv refusals
        "the reason-carrying check as handlers reach it (A2)."),
    "ks_user_addr_ok": (
        4,   # definition, declaration, a signal handler's address (H-017), TLS base
        "the address policy as handlers reach it (A2). The sigreturn rip check "
        "(H-018) moved with the frame into ks_sigframe_pop, where it calls "
        "hw_user_addr_ok itself."),
    "hw_user_addr_ok": (
        5,   # 6 -> 5 in A2: two handler calls became ks_user_addr_ok, and ksvc.c adds
             # the wrapper's and the sigreturn frame's
        "address policy for the two user windows. VibeOS programs link at "
        "0x8000000000 and Linux ones at 0x400000, inside the kernel's identity "
        "map, so 'is this a user address' is not a range test anybody should "
        "write twice. 2 -> 4 with the user-access recovery (H-003, H-010): a "
        "forward declaration, and the trap handler asking it before it resumes "
        "a faulted copy. 4 -> 6 validating a signal handler (H-017) and a "
        "restored sigreturn rip (H-018) canonical before either can reach "
        "iretq, where a non-canonical rip faults in ring 0."),
    "hw_task_alloc_guarded": (
        4,   # 3 -> 4 when C5 moved the task lifecycle to task_life.c (fork's caller):
             # the function crossed a file, so it stopped being static and gained a
             # header declaration. Same two callers as before, one more site to say so.
        "the fork guard. A task table that can be exhausted by a loop is a "
        "denial of service with no privilege required."),
}


def sources():
    out = []
    for d in SCOPE:
        for base, _, names in os.walk(os.path.join(ROOT, d)):
            if "build" in base or ".git" in base:
                continue
            for n in names:
                if n.endswith((".c", ".h")):
                    out.append(os.path.join(base, n))
    return out


def main():
    files = sources()
    text = {}
    for f in files:
        try:
            text[f] = open(f, encoding="utf-8", errors="replace").read()
        except OSError:
            text[f] = ""

    bad = []
    for name, (want, why) in sorted(CHOKEPOINTS.items()):
        pat = re.compile(r"\b" + re.escape(name) + r"\s*\(")
        got = 0
        where = []
        for f, t in text.items():
            n = len(pat.findall(t))
            if n:
                got += n
                where.append("%s:%d" % (os.path.relpath(f, ROOT), n))
        if "--list" in sys.argv:
            print("  %-24s %2d (declared %2d)  %s"
                  % (name, got, want, " ".join(where)))
        if got != want:
            bad.append((name, got, want, why))

    if bad:
        for name, got, want, why in bad:
            direction = "more" if got > want else "FEWER"
            print("  %s: %d call sites, declared %d - %s than declared"
                  % (name, got, want, direction))
            print("      %s" % why)
        # Advice first, verdict last: check.sh reads this with `| tail -1`, so
        # anything printed after the verdict replaces it in the summary. This
        # file had the wrong order from the day it was written and never showed
        # it, because it has never gone red inside check.sh.
        print("      If the change is deliberate, update the count in "
              "scripts/dev/check-chokepoints.py in the same commit.")
        print("chokepoints=FAIL moved=%d" % len(bad))
        return 1
    print("chokepoints=ok watched=%d" % len(CHOKEPOINTS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
