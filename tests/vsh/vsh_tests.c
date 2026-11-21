/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vsh's language on the host (docs/shell/s4.md): what each text parses to, what
 * it refuses, what words and conditions evaluate to, and random text that must
 * neither crash the parser nor make it run away. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vsh.h"

static int g_failed, g_checks;

static void check(int ok, const char *what, const char *detail) {
    g_checks++;
    if (!ok) {
        g_failed++;
        printf("FAIL:vsh %s%s%s\n", what, detail ? " -> " : "", detail ? detail : "");
    }
}

static void parses(const char *src, const char *want) {
    vsh_arena_t a;
    vsh_sb_t o = {0, 0, 0};
    char err[128];
    vsh_node_t *n;

    vsh_arena_init(&a);
    n = vsh_parse(&a, src, err, sizeof(err), 0);
    if (err[0]) {
        check(0, src, err);
    } else {
        vsh_dump(n, &o);
        check(strcmp(o.b, want) == 0, src, o.b);
    }
    vsh_sb_free(&o);
    vsh_arena_free(&a);
}

static void refuses(const char *src, const char *why) {
    vsh_arena_t a;
    char err[128];

    vsh_arena_init(&a);
    (void)vsh_parse(&a, src, err, sizeof(err), 0);
    check(err[0] != 0 && strstr(err, why) != 0, src, err[0] ? err : "accepted");
    vsh_arena_free(&a);
}

/* ---- a small world for the evaluators -------------------------------------- */

static const char *const g_list[] = {"a b", "c"};
static const char *const g_empty_list[] = {"x"};
static int g_ran;

static int w_lookup(void *u, const char *name, vsh_value_t *out) {
    static const struct {
        const char *n, *v;
    } vars[] = {
        {"x", "1"}, {"ten", "10"}, {"nine", "9"}, {"sp", "two words"}, {"star", "*"},
        {"name", "Report.TXT"}, {"empty", ""}, {"big", "9223372036854775807"},
        {"low", "-9223372036854775808"}, {"word", "abc"},
    };
    static const char *one[1];
    size_t i;

    (void)u;
    if (strcmp(name, "l") == 0) {
        out->count = 2;
        out->items = g_list;
        out->is_list = 1;
        return 0;
    }
    if (strcmp(name, "nolist") == 0) {
        out->count = 0;
        out->items = g_empty_list;
        out->is_list = 1;
        return 0;
    }
    for (i = 0; i < sizeof(vars) / sizeof(vars[0]); i++) {
        if (strcmp(vars[i].n, name) == 0) {
            one[0] = vars[i].v;
            out->count = 1;
            out->items = one;
            out->is_list = 0;
            return 0;
        }
    }
    return -1;
}

static int w_capture(void *u, const vsh_node_t *script, char **out) {
    vsh_ctx_t *c = (vsh_ctx_t *)u;

    (void)script;
    *out = vsh_strndup(c->arena, "out put\n\n", 9);
    return 0;
}

static int w_run(void *u, const vsh_node_t *chain, int *status) {
    vsh_sb_t o = {0, 0, 0};

    (void)u;
    g_ran++;
    vsh_dump(chain, &o);
    *status = strstr(o.b, "fails") ? 1 : 0;
    vsh_sb_free(&o);
    return 0;
}

static void w_ctx(vsh_ctx_t *c, vsh_arena_t *a) {
    memset(c, 0, sizeof(*c));
    c->arena = a;
    c->user = c;
    c->status = 7;
    c->lookup = w_lookup;
    c->capture = w_capture;
    c->run = w_run;
}

/* The words of `cmd <text>`, joined by '|'. */
static void expands(const char *text, const char *want) {
    vsh_arena_t a;
    vsh_ctx_t c;
    vsh_list_t l = {0, 0, 0};
    vsh_sb_t src = {0, 0, 0}, got = {0, 0, 0};
    char err[128];
    vsh_node_t *n;
    const vsh_word_t *w;
    size_t i;
    int bad = 0;

    vsh_arena_init(&a);
    w_ctx(&c, &a);
    vsh_sb_puts(&src, "cmd ");
    vsh_sb_puts(&src, text);
    n = vsh_parse(&a, src.b, err, sizeof(err), 0);
    if (!n) {
        check(0, text, err);
    } else {
        for (w = n->a->words->next; w; w = w->next) {
            bad |= vsh_expand_word(&c, w, &l) != 0;
        }
        vsh_sb_putn(&got, "", 0u);
        for (i = 0; i < l.count; i++) {
            if (i) {
                vsh_sb_putc(&got, '|');
            }
            vsh_sb_puts(&got, l.items[i]);
        }
        check(!bad && strcmp(got.b, want) == 0, text, bad ? c.err : got.b);
    }
    vsh_sb_free(&src);
    vsh_sb_free(&got);
    vsh_arena_free(&a);
}

/* `if <text> { y }`: 1 true, 0 false, -1 an evaluation error. */
static int cond_of(const char *text) {
    vsh_arena_t a;
    vsh_ctx_t c;
    vsh_sb_t src = {0, 0, 0};
    char err[128];
    vsh_node_t *n;
    int r = -1, out = 0;

    vsh_arena_init(&a);
    w_ctx(&c, &a);
    vsh_sb_puts(&src, "if ");
    vsh_sb_puts(&src, text);
    vsh_sb_puts(&src, " { y }");
    n = vsh_parse(&a, src.b, err, sizeof(err), 0);
    if (!n) {
        check(0, text, err);
    } else if (vsh_cond(&c, n->a, &out) == 0) {
        r = out != 0;
    }
    vsh_sb_free(&src);
    vsh_arena_free(&a);
    return r;
}

static void cond_is(const char *text, int want) {
    char got[16];

    snprintf(got, sizeof(got), "%d", cond_of(text));
    check(cond_of(text) == want, text, got);
}

/* ---- the groups ------------------------------------------------------------ */

static void t_statements(void) {
    parses("", "");
    parses("  \n ; \n# nothing\n", "");
    parses("set x = 1", "set x = 1");
    parses("set x=1", "set x = 1");
    parses("SET x = 1", "set x = 1");
    parses("set l = (a \"b c\"\n d)", "set l = (a 'b c' d)");
    parses("set l = ()", "set l = ()");
    parses("global g = $x", "global g = $x");
    parses("export E", "export E");
    parses("export E = v", "export E = v");
    parses("set -e off; set -e on", "errors off ; errors on");
    parses("if $a == 1 { b } else if $c { d } else { e }",
           "if ($a == 1) { cmd[b] } else if $c { cmd[d] } else { cmd[e] }");
    parses("if a {\n b\n}\nelse {\n c\n}", "if a { cmd[b] } else { cmd[c] }");
    parses("if a { b }\nc", "if a { cmd[b] } ; cmd[c]");
    parses("for i in a \"b c\" *.c { x $i }", "for i in a 'b c' *.c { cmd[x $i] }");
    parses("while $n > 0 { break; continue }", "while ($n > 0) { break ; continue }");
    parses("func f a b { return 3 }", "func f(a b) { return 3 }");
    parses("func f { return }", "func f() { return }");
    /* An empty else is no else. */
    parses("if a {} else {}", "if a {  }");
}

static void t_commands(void) {
    parses("a b | c d", "cmd[a b] | cmd[c d]");
    parses("a && b || c", "((cmd[a] && cmd[b]) || cmd[c])");
    parses("a &", "cmd[a] &");
    parses("a && b &", "(cmd[a] && cmd[b]) &");
    parses("a; b", "cmd[a] ; cmd[b]");
    parses("try rm f", "try cmd[rm f]");
    parses("trying x", "cmd[trying x]");
    parses("a > o 2>&1", "cmd[a 1>o 2>&1]");
    parses("a >> o < i 2> e", "cmd[a 1>>o 0<i 2>e]");
    parses("a 2>e", "cmd[a 2>e]");
    parses("a b2 c", "cmd[a b2 c]");
    parses("X=1 Y=\"a b\" cmd arg", "cmd[X=1 Y='a b' cmd arg]");
    parses("X= cmd", "cmd[X='' cmd]");
    parses("grep a=b f", "cmd[grep a=b f]");
    parses("find . -exec {} ;", "cmd[find . -exec {}]");
    parses("echo a{b}", "cmd[echo a{b}]");
    parses("echo }x", "cmd[echo }x]");
    parses("if a { echo }x }", "if a { cmd[echo }x] }");
    parses("a |\n b", "cmd[a] | cmd[b]");
    parses("a &&\n b", "(cmd[a] && cmd[b])");
    parses("a \\\n b", "cmd[a b]");
    parses("a # comment", "cmd[a]");
    parses("a#b", "cmd[a#b]");
}

static void t_words(void) {
    parses("e 'a $b'", "cmd[e 'a $b']");
    parses("e \"a $b c\"", "cmd[e 'a '\"$b\"' c']");
    parses("e $a${b}c", "cmd[e $a$bc]");
    parses("e ${f[2]}", "cmd[e $f[2]]");
    parses("e %CD%", "cmd[e %CD%]");
    parses("date +%d%m%Y", "cmd[date +%d%m%Y]");
    parses("e 100%", "cmd[e 100%]");
    parses("e $(a | b)", "cmd[e $(cmd[a] | cmd[b])]");
    parses("e $(a $(b))", "cmd[e $(cmd[a $(cmd[b])])]");
    parses("e $(( $n + (1 * 2) ))", "cmd[e $(( $n + (1 * 2) ))]");
    parses("e $?", "cmd[e $?]");
    parses("e \"\"", "cmd[e '']");
    parses("e $", "cmd[e $]");
    /* The backslash escapes only what the grammar says. */
    parses("e C:\\Users\\vibe", "cmd[e C:\\Users\\vibe]");
    parses("e a\\ b", "cmd[e a b]");
    parses("e \\$x", "cmd[e $x]");
    parses("e \\\\", "cmd[e \\]");
    parses("e \"\\$x \\\" \\n\"", "cmd[e '$x \" \\n']");
}

static void t_refusals(void) {
    refuses("x=1", "set x = 1");
    refuses("set x", "= expected");
    refuses("set x =", "a value expected");
    refuses("set -e maybe", "on or off");
    refuses("if a", "{ expected");
    refuses("if { b }", "a condition expected");
    refuses("if a { b", "missing }");
    refuses("if a == { b }", "{ expected");
    refuses("}", "without {");
    refuses("else { a }", "else without if");
    refuses("for i a { b }", "for takes");
    refuses("func { a }", "a function name");
    refuses("e 'a", "missing closing '");
    refuses("e \"a", "missing closing \"");
    refuses("e $(a", "missing )");
    refuses("e $((1", "missing ))");
    refuses("e ${a", "missing }");
    refuses("e ${a[x]}", "an index");
    refuses("a >", "a file name");
    refuses("a 2>&x", "a descriptor");
    refuses("a |", "a command expected");
    refuses("a &&", "a command expected");
    refuses("if a { b } c", "unexpected text");
}

static void t_expansion(void) {
    expands("$x", "1");
    /* No word splitting: this is the rule the language exists for. */
    expands("$sp", "two words");
    expands("$l", "a b|c");
    expands("\"$l\"", "a b c");
    expands("p$l", "pa b c");
    expands("${l[0]} ${l[1]} [${l[9]}]", "a b|c|[]");
    expands("${x[0]}[${x[1]}]", "1[]");
    expands("$nolist", "");
    expands("$unset", "");
    expands("[$unset]", "[]");
    expands("%x%", "1");
    expands("\"%sp%\"", "two words");
    expands("%unset%", "%unset%");
    expands("+%d%m%Y", "+%d%m%Y");
    expands("$?", "7");
    expands("$(a)", "out put");
    expands("\"[$(a)]\"", "[out put]");
    expands("$(( 1 + 2 * 3 ))", "7");
    expands("$(( (1 + 2) * 3 ))", "9");
    expands("$(( $ten / 3 ))", "3");
    expands("$(( ten % 3 ))", "1");
    expands("$(( -$x + --2 ))", "1");
    expands("'$x' \\$x", "$x|$x");
}

static void t_arith(void) {
    static const char *const bad[] = {
        "1 / 0", "1 % 0", "word + 1", "unset", "l", "1 +", "(1", "1 2", "99999999999999999999",
    };
    vsh_arena_t a;
    vsh_ctx_t c;
    int64_t r = 0;
    size_t i;

    vsh_arena_init(&a);
    w_ctx(&c, &a);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        check(vsh_arith(&c, bad[i], &r) != 0, "arithmetic is refused", bad[i]);
    }
    check(vsh_arith(&c, "low / -1", &r) == 0 && r == INT64_MIN, "INT64_MIN / -1 does not trap", 0);
    check(vsh_arith(&c, "low % -1", &r) == 0 && r == 0, "INT64_MIN % -1 does not trap", 0);
    check(vsh_arith(&c, "big + 1", &r) == 0 && r == INT64_MIN, "overflow wraps", 0);
    vsh_arena_free(&a);
}

static void t_conditions(void) {
    cond_is("$x == 1", 1);
    cond_is("$x != 1", 0);
    /* Numbers as numbers, text as text. */
    cond_is("$ten > $nine", 1);
    cond_is("$ten < $nine", 0);
    cond_is("10 == 010", 1);
    cond_is("abc < abd", 1);
    cond_is("10 < abc", 1);
    cond_is("$big < 9223372036854775808", 1);
    cond_is("5 <= 5", 1);
    cond_is("5 >= 6", 0);
    cond_is("Abc == abc", 0);
    cond_is("$name ~ *.TXT", 1);
    cond_is("$name ~ *.txt", 0);
    cond_is("$name ~~ *.txt", 1);
    cond_is("$name ~~ [q-s]*", 1);
    cond_is("$name ~ [!R]*", 0);
    cond_is("$name ~ \"*.TXT\"", 0);
    cond_is("'*.TXT' ~ \"*.TXT\"", 1);
    cond_is("$star ~ $star", 1);
    cond_is("abc ~ $star", 1);
    cond_is("abc ~ \"$star\"", 0);
    cond_is("$x", 1);
    cond_is("$empty", 0);
    cond_is("$unset", 0);
    cond_is("!$empty", 1);
    cond_is("$x == 1 && $ten == 10", 1);
    cond_is("$x == 2 || $ten == 10", 1);
    cond_is("$x == 2 && $ten == 10", 0);
    cond_is("$x==1", 1);
    cond_is("$x!=1", 0);
    cond_is("(ok)", 1);
    cond_is("(fails)", 0);
    cond_is("! (fails)", 1);
    cond_is("(ok && ok) && $x == 1", 1);

    /* The right side of a decided condition must not run: it may be a command
     * with an effect. */
    g_ran = 0;
    (void)cond_of("$x == 2 && (ok)");
    check(g_ran == 0, "&& does not run its right side after a false left", 0);
    (void)cond_of("$x == 1 || (ok)");
    check(g_ran == 0, "|| does not run its right side after a true left", 0);
    (void)cond_of("$x == 1 && (ok)");
    check(g_ran == 1, "&& runs its right side after a true left", 0);
}

static void t_match(void) {
    check(vsh_match("a*b*c", "aXXbYYc", 0), "stars match", 0);
    check(!vsh_match("a*b", "aXc", 0), "a star does not excuse the end", 0);
    check(vsh_match("a?c", "abc", 0) && !vsh_match("a?c", "ac", 0), "? is one character", 0);
    check(vsh_match("[a-c]x", "bx", 0) && !vsh_match("[a-c]x", "dx", 0), "a class", 0);
    check(vsh_match("[]]", "]", 0), "] first in a class is a member", 0);
    check(vsh_match("[a", "[a", 0), "an unclosed [ is a [", 0);
    check(vsh_match("\\*", "*", 0) && !vsh_match("\\*", "a", 0), "an escaped star is a star", 0);
    check(vsh_match("", "", 0) && !vsh_match("", "a", 0), "the empty pattern", 0);
    check(vsh_match("*", "", 0), "a star matches nothing", 0);
    {
        /* Exponential in the textbook recursion. */
        char text[4097];
        char pat[201];
        size_t i;

        memset(text, 'a', sizeof(text) - 1u);
        text[sizeof(text) - 1u] = 0;
        for (i = 0; i + 3u < sizeof(pat); i += 2u) {
            pat[i] = 'a';
            pat[i + 1u] = '*';
        }
        pat[i] = 'b';
        pat[i + 1u] = 0;
        check(!vsh_match(pat, text, 0), "a*a*a*...b ends", 0);
    }
}

static void t_limits(void) {
    enum { N = 200000 };
    char *deep = (char *)malloc(N * 3u + 8u);
    size_t i;

    /* Each of these is a stack overflow if the bound goes. */
    memcpy(deep, "e ", 2u);
    for (i = 0; i < N; i++) {
        memcpy(deep + 2u + i * 2u, "$(", 2u);
    }
    deep[2u + N * 2u] = 0;
    refuses(deep, "nested too deeply");

    deep[0] = 0;
    for (i = 0; i < N / 2; i++) {
        memcpy(deep + i * 3u, "a&&", 3u);
    }
    memcpy(deep + (N / 2) * 3u, "a", 2u);
    refuses(deep, "chain too long");

    memcpy(deep, "if a { b }", 10u);
    for (i = 0; i < N / 20; i++) {
        memcpy(deep + 10u + i * 19u, " else if a { b }   ", 19u);
    }
    deep[10u + (N / 20) * 19u] = 0;
    refuses(deep, "too many else if");
    free(deep);
}

/* Random text from the characters the grammar cares about: refused or parsed,
 * never a crash, and whatever parsed can be dumped and evaluated. The sanitizer
 * job is what makes this mean more than "did not segfault". */
static void t_random(uint32_t seed, uint32_t rounds) {
    static const char *const atoms[] = {
        "if ", "else ", "for ", "in ", "while ", "func ", "set ", "global ", "export ", "try ",
        "return ", "break", "{ ", " }", "(", ")", "$(", "$((", "))", "${", "}", "[", "]", "$", "%",
        "\"", "'", "\\", "\n", ";", "|", "||", "&", "&&", "<", ">", ">>", "2>&1", "==", "!=", "~",
        "~~", "!", "=", "x", "1", " ", "*", "-e ", "on", "%x%", "$x", "$l", "$?",
    };
    uint32_t r, state = seed ? seed : 1u;

    for (r = 0; r < rounds; r++) {
        vsh_arena_t a;
        vsh_ctx_t c;
        vsh_sb_t src = {0, 0, 0}, out = {0, 0, 0};
        char err[128];
        vsh_node_t *n, *s;
        uint32_t len, i;

        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        len = state % 24u;
        vsh_sb_putn(&src, "", 0u);
        for (i = 0; i < len; i++) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            vsh_sb_puts(&src, atoms[state % (sizeof(atoms) / sizeof(atoms[0]))]);
        }
        vsh_arena_init(&a);
        w_ctx(&c, &a);
        n = vsh_parse(&a, src.b, err, sizeof(err), 0);
        check(!(n && err[0]), "a parse has a tree or a reason, not both", src.b);
        vsh_dump(n, &out);
        for (s = n; s; s = s->next) {
            int v;

            if (s->kind == VSH_N_IF || s->kind == VSH_N_WHILE) {
                (void)vsh_cond(&c, s->a, &v);
            } else if (s->kind == VSH_N_PIPELINE && s->a && s->a->words) {
                vsh_list_t l = {0, 0, 0};
                const vsh_word_t *w;

                for (w = s->a->words; w; w = w->next) {
                    (void)vsh_expand_word(&c, w, &l);
                }
            }
        }
        vsh_sb_free(&out);
        vsh_sb_free(&src);
        vsh_arena_free(&a);
    }
}

int main(int argc, char **argv) {
    uint32_t seed = argc > 1 ? (uint32_t)strtoul(argv[1], 0, 10) : 1u;
    uint32_t rounds = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 10) : 20000u;

    t_statements();
    t_commands();
    t_words();
    t_refusals();
    t_expansion();
    t_arith();
    t_conditions();
    t_match();
    t_limits();
    t_random(seed, rounds);
    printf("vsh: %d checks, %d failed (seed %u, %u random scripts)\n", g_checks, g_failed, seed, rounds);
    return g_failed ? 1 : 0;
}
