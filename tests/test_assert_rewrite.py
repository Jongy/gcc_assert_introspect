import os.path
import subprocess
import shutil
import signal
from tempfile import NamedTemporaryFile, mkdtemp
import re
from termcolor import colored, RESET
import pytest

from conftest import GCC, OUTPUT_FILE


ASSERT_INTROSPECT_SO = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "assert_introspect.so"))

HEADERS = "#include <assert.h>\n#include <stdio.h>\n#include <stdlib.h>\n"

ANSI_ESCAPE = re.compile(r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])')


def run_tester(opt_level, test_prototype, test_code, calling_code, *, extra_test="",
               skip_first=True, strip_colors=True, compile_error=False):
    with NamedTemporaryFile("w", suffix=".c") as test, \
         NamedTemporaryFile("w", suffix=".c") as caller, \
         NamedTemporaryFile(suffix=".o") as obj:

        test.write(HEADERS + extra_test + "{0} {{ {1} }}".format(test_prototype, test_code))
        test.flush()
        extra_opts = ["-Werror", "-Wall", ] + ([opt_level] if opt_level else [])
        try:
            output = subprocess.check_output([GCC,
                                             "-fplugin={}".format(ASSERT_INTROSPECT_SO),
                                             "-c", test.name, "-o", obj.name] + extra_opts,
                                             stderr=subprocess.PIPE)
            assert not compile_error, "compilation should have failed!\n"
        except subprocess.CalledProcessError as e:
            assert compile_error, "compilation failed unexpectedly!\n" + e.stderr.decode()
            return e.stderr.decode()
        else:
            lines = output.decode().splitlines()
            assert len(lines) == 1 and lines[0].startswith("assert_introspect loaded"), output

        caller.write(HEADERS + "{0}; int main(void) {{ setlinebuf(stdout); {1}; return 0; }}"
            .format(test_prototype, calling_code))
        caller.flush()
        output_dir = mkdtemp()
        try:
            output_file = os.path.join(output_dir, "out")

            subprocess.check_call([GCC, "-o", output_file, obj.name, caller.name])
            with pytest.raises(subprocess.CalledProcessError) as e:
                subprocess.check_output([output_file])

            output = e.value.output.decode()
            assert e.value.returncode == -signal.SIGABRT.value, "output: " + output

            OUTPUT_FILE.write(output + "------------\n")

            if strip_colors:
                output = ANSI_ESCAPE.sub("", output)

            return output.splitlines()[1 if skip_first else 0:]
        finally:
            shutil.rmtree(output_dir)


@pytest.fixture(params=[None, "-O2", "-O3"])
def opt_level(request):
    return request.param


# short names so colored expressions don't get too long.
br = lambda s: colored(s, "red", attrs=["bold"])
bb = lambda s: colored(s, "blue", attrs=["bold"])
bg = lambda s: colored(s, "green", attrs=["bold"])
bgr = lambda s: bg(s)[:-len(RESET)]
by = lambda s: colored(s, "yellow", attrs=["bold"])
byr = lambda s: by(s)[:-len(RESET)]
bm = lambda s: colored(s, "magenta", attrs=["bold"])
bmr = lambda s: bm(s)[:-len(RESET)]
bc = lambda s: colored(s, "cyan", attrs=["bold"])
bcr = lambda s: bc(s)[:-len(RESET)]
dr = lambda s: colored(s, "red", attrs=["dark"])


def test_sanity(opt_level):
    out = run_tester(opt_level, "void test(int n)", "assert(n == 5);", "test(3);")
    assert out == [
        "> assert(n == 5)",
        "A assert(n == 5)",
        "E assert(3 == 5)",
        "> subexpressions:",
        "  n = 3",
    ]


def test_logical_and_expression_right_repr(opt_level):
    """
    if the left side of an AND expression passed but the right side failed, only
    the right side is shown.
    test_subexpression_not_evaluated tests the opposite case (left side fails)

    variables of the left side are not shown, as well!
    """
    out = run_tester(opt_level, "void test(int n, int m)", 'assert(n == 42 && m == 7);',
                     'test(42, 6);')
    assert out == [
        "> assert(n == 42 && m == 7)",
        "A assert((n == 42) && (m == 7))",
        "E assert((...) && (6 == 7))",
        "> subexpressions:",
        "  m = 6",
    ]


def test_logical_or_expression_repr_both(opt_level):
    """
    if both sides of an OR expression fail, both are printed (different logic from AND)
    """
    out = run_tester(opt_level, "void test(int n, int m)", 'assert(n == 43 || m == 7);',
                     'test(42, 6);')
    assert out == [
        "> assert(n == 43 || m == 7)",
        "A assert((n == 43) || (m == 7))",
        "E assert((42 == 43) || (6 == 7))",
        "> subexpressions:",
        "  n = 42",
        "  m = 6",
    ]


def test_subexpression_function_call_repr(opt_level):
    """
    tests the generated function call repr.
    1. functions with 0, 1, 2 arguments
    2. function calls inside function calls
    3. subexpressions are displayed in their evaluation order.
    """
    out = run_tester(opt_level, "void test(int n)", "assert(f2(f1(f0()), n) == 5);", "test(20);",
                     extra_test="int f2(int m, int n) { return m + n; }\n"
                                "int f1(int n) { return n - 1; }\n"
                                "int f0(void) { return 5; }", strip_colors=False)
    assert out == [
        "> assert(f2(f1(f0()), n) == 5)",
        f"{bb('A')} assert({bgr('f2(') + byr('f1(') + bm('f0()') + by(')') + bgr(', ') + bc('n') + bg(')')} == 5)",
        f"{br('E')} assert({bg('24')} == 5)",
        "> subexpressions:",
        # note - expressions are ordered in their evaluation order,
        # f0 -> f1 -> n -> f2
        f"  {bm('f0() = 5')}",
        f"  {byr('f1(') + bmr('5') + by(') = 4')}",
        f"  {bc('n = 20')}",
        f"  {bgr('f2(') + byr('4') + bgr(', ') + bcr('20') + bg(') = 24')}",
    ]


def test_subexpression_string_repr(opt_level):
    """
    tests "string pointers" are identified and their repr use %s.
    also tests that NULL is *not* identified as a string pointer, but is identified
    as NULL in the AST-rebuilt expression.
    """
    out = run_tester(opt_level, "void test(const char *s)", 'assert(strstr("hello world", s) == NULL);',
                     'test("world");', extra_test="#include <string.h>\n")
    assert out == [
        '> assert(strstr("hello world", s) == NULL)',
        'A assert(strstr("hello world", s) == NULL)',
        'E assert("world" == (nil))',
        '> subexpressions:',
        '  s = "world"',
        '  strstr("hello world", "world") = "world"',
    ]


def test_subexpression_evaluated_once(opt_level):
    """
    tests that subexpressions in the assert are evaluated only once (due to the use of save_expr)
    even though we show them multiple times (once in the assert repr, and once more in the
    call_me_once call repr).
    """
    extra = """
    int call_me_once(int n) {
        static int called = 0;

        if (called) {
            printf("call_me_once evaluated more than once!\\n");
            exit(1); // will fail the SIGABRT assert in pytest
        }
        called = 1;

        return n + 1;
    }
    """

    out = run_tester(opt_level, "void test(int n)", 'assert(call_me_once(n) == n);',
                     'test(3);', extra_test=extra)
    assert out == [
        "> assert(call_me_once(n) == n)",
        "A assert(call_me_once(n) == n)",
        "E assert(4 == 3)",
        "> subexpressions:",
        "  n = 3",
        "  call_me_once(3) = 4",
    ]


def test_subexpression_not_evaluated(opt_level):
    """
    tests that a subexpression not evaluated by the original condition, will not be evaluated
    when we repr it.
    i.e, assert(0 && dont_call_me()) should call that function.
    this actually expands what test_and_expression_short_circut tests.
    """

    extra = """
    int dont_call_me(int n) {
        printf("dont_call_me was evaluated!\\n");
        exit(1); // will fail the SIGABRT assert in pytest
    }
    """

    out = run_tester(opt_level, "void test(int n)", 'assert(n == 5 && dont_call_me(n) == n);',
                     'test(42);', extra_test=extra)
    assert out == [
        "> assert(n == 5 && dont_call_me(n) == n)",
        "A assert((n == 5) && (dont_call_me(n) == n))",
        "E assert(42 == 5)",
        "> subexpressions:",
        "  n = 42",
    ]


def test_subexpression_colors(opt_level):
    """
    tests color assigning to subexpressions:
    1. variables get colors
    2. casted variables get different colors
    3. function calls get colors
    4. subexpressions inside function calls are colored with their color
    5. parts of complex subexpressions inside function calls are colored with their color
       (variables + function calls part of a binary expression) + added to subexpressions

    also tests the colorizing of A and E.
    also tests AST casts.
    """
    extra = """
    int func5(int n) {
        return 7;
    }
    """

    out = run_tester(opt_level, "void test(int n)", 'assert(n == 5 || (short)n == 6 || func5(n) == n || func5(n + func5(9)) == 12);',
                     'test(42);', extra_test=extra, strip_colors=False)

    assert out == [
        "> assert(n == 5 || (short)n == 6 || func5(n) == n || func5(n + func5(9)) == 12)",
        f"{bb('A')} assert(((({bg('n')} == 5) || ({by('(short int)n')} == 6)) || "
            f"({bmr('func5(') + bg('n') + bm(')')} == {bg('n')})) || ({bcr('func5(') + dr('func5(9)') + ' + ' + bg('n') + bc(')')} == 12))",
        f"{br('E')} assert(((({bg('42')} == 5) || ({by('42')} == 6)) || ({bm('7')} == {bg('42')}))"
            f" || ({bc('7')} == 12))",
        "> subexpressions:",
        f"  {bg('n = 42')}",
        f"  {by('(short int)n = 42')}",
        # necessary to strip the RESET because the plugin doesn't emit those if it knows
        # the next part is colored anyway.
        f"  {bmr('func5(') + bgr('42') + bm(') = 7')}",
        f"  {dr('func5(9) = 7')}",
        f"  {bc('func5(49) = 7')}",
    ]


def test_ast_double_cast(opt_level):
    """
    further tests that casts are displayed in the AST repr and also displayed in variable
    subexpressions, makes sure "double casts" (cast then promote) are displayed:
    that is, NOP_EXPR(CONVERT_EXPR(variable)), for example, in
    "short x = n; x + 5 == (short)n;" everything is promoted to int:
    "(int)x + 5 == (int)(short int)n".
    """

    out = run_tester(opt_level, "void test(int n)", 'short x = n; assert(x + 5 == (short)n);', 'test(5);')
    assert out == [
        "> assert(x + 5 == (short)n)",
        # double cast
        "A assert((int)x + 5 == (int)(short int)n)",
        "E assert(5 + 5 == 5)",
        "> subexpressions:",
        "  (int)x = 5",
        "  (int)(short int)n = 5",
    ]


def test_error_in_expression(opt_level):
    """
    if there's an error inside the assert expression, the plugin should identify it and refuse to
    rewrite.
    """
    out = run_tester(opt_level, "void test(int n)", 'assert(n == m);', 'test(5);', compile_error=True)
    assert "error: assert_introspect: previous error in expression, not rewriting assert" in out


def test_binary_expression_casts_skipped(opt_level):
    """
    tests that casts on binary expressions are bypassed. for example:

        int x = 5, y = 7;
        unsigned long n = x;
        assert(n == x + y);

    in this case, the PLUS_EXR 'x + y' is wrapped in a NOP_EXPR to cast it to "long unsigned int".
    make sure the plugin identifies it and sees ahead.
    """
    out = run_tester(opt_level, "void test(int n)", 'unsigned long x = n; assert(x + 8 == n + 2);', 'test(5);')
    assert out == [
        "> assert(x + 8 == n + 2)",
        "A assert(x + 8 == n + 2)",
        "E assert(5 + 8 == 5 + 2)",
        "> subexpressions:",
        "  x = 5",
        "  n = 5",
    ]


def test_ast_repr_addressof(opt_level):
    """
    tests the printing of &variable
    """
    out = run_tester(opt_level, "void test(int n)", 'assert(!func5(&n));', 'test(5);',
                     extra_test="int func5(int *n) { return *n + 5; }", strip_colors=False)
    assert out[:-1] == [
        "> assert(!func5(&n))",
        f"{bb('A')} assert({bg('func5(&n)')} == 0)",
        f"{br('E')} assert({bg('10')} == 0)",
        "> subexpressions:",
    ]
    # it's a real hassle to test a regex with colors
    assert re.match(r"  func5\(0x[a-f0-9]+\) = 10", ANSI_ESCAPE.sub("", out[-1]))


def test_subexpression_var_not_evaluated(opt_level):
    """
    variables that are part of an expression not evaluated should not be displayed.
    """

    out = run_tester(opt_level, "void test(int n, int m)", 'assert(n == 41 && m == 6);',
                     'test(42, 6);')
    assert out == [
        "> assert(n == 41 && m == 6)",
        "A assert((n == 41) && (m == 6))",
        "E assert(42 == 41)",
        "> subexpressions:",
        "  n = 42",
    ]


def test_ast_unknown_expr(opt_level):
    """
    tests that expressions we don't know how to parse (yet) are printed as "..." in the AST repr, and
    we don't crash.
    ultimately I want this test to be empty ^^

    currently:
    * struct . reference
    * struct -> reference
    * array access

    """

    extra = """
    struct c {
        struct {
            int a;
        } b;
    };

    int func(int n) {
        return n + 5;
    }
    """

    test_code = """
    struct c c;
    c.b.a = 5;

    struct c *cp = &c;

    int arr[] = { 5, 5 };

    assert(func(c.b.a + n) == 42 || cp->b.a == 12 || arr[1] + 2 == 4 * arr[0]);
    """

    out = run_tester(opt_level, "void test(int n)", test_code, 'test(6);', extra_test=extra)
    assert out == [
        "> assert(func(c.b.a + n) == 42 || cp->b.a == 12 || arr[1] + 2 == 4 * arr[0])",
        "A assert(((func(... + n) == 42) || (... == 12)) || (... + 2 == ... * 4))",
        "E assert(((16 == 42) || (5 == 12)) || (5 + 2 == 5 * 4))",
        "> subexpressions:",
        "  n = 6",
        "  func(11) = 16",
    ]
