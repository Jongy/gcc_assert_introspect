import os.path
import subprocess
import shutil
import signal
from tempfile import NamedTemporaryFile, mkdtemp
import pytest

from conftest import GCC


ASSERT_INTROSPECT_SO = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "assert_introspect.so"))

HEADERS = "#include <assert.h>\n#include <stdio.h>\n#include <stdlib.h>\n"


def run_tester(test_prototype, test_code, calling_code, *, extra_test="",
               skip_first=True):
    with NamedTemporaryFile("w", suffix=".c") as test, \
         NamedTemporaryFile("w", suffix=".c") as caller, \
         NamedTemporaryFile(suffix=".o") as obj:

        test.write(HEADERS + extra_test + "{0} {{ {1} }}".format(test_prototype, test_code))
        test.flush()
        extra_opts = ["-Werror", "-Wall", ]
        subprocess.check_call([GCC,
                               "-fplugin={}".format(ASSERT_INTROSPECT_SO),
                               "-c", test.name, "-o", obj.name] + extra_opts)

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

            return output.splitlines()[1 if skip_first else 0:]
        finally:
            shutil.rmtree(output_dir)


def test_sanity():
    out = run_tester("void test(int n)", "assert(n == 5);", "test(3);")
    assert out == [
        "> assert(n == 5)",
        "  assert(3 == 5)",
        "> subexpressions:",
        "  3 = n",
    ]


def test_logical_and_expression_right_repr():
    """
    if the left side of an AND expression passed but the right side failed, only
    the right side is shown.
    test_subexpression_not_evaluated tests the opposite case (left side fails)
    """
    out = run_tester("void test(int n, int m)", 'assert(n == 42 && m == 7);',
                     'test(42, 6);')
    assert out == [
        "> assert(n == 42 && m == 7)",
        "  assert((...) && (6 == 7))",
        "> subexpressions:",
        "  42 = n",  # TODO n shouldn't be here - should show variables only if they're relevant.
        "  6 = m",
    ]


def test_logical_or_expression_repr_both():
    """
    if both sides of an OR expression fail, both are printed (different logic from AND)
    """
    out = run_tester("void test(int n, int m)", 'assert(n == 43 || m == 7);',
                     'test(42, 6);')
    assert out == [
        "> assert(n == 43 || m == 7)",
        "  assert((42 == 43) || (6 == 7))",
        "> subexpressions:",
        "  42 = n",
        "  6 = m",
    ]


def test_subexpression_function_call_repr():
    """
    tests the generated function call repr
    """
    out = run_tester("void test(int n)", "assert(f(12, n) == 5);", "test(20);",
                     extra_test="int f(int m, int n) { return m + n; }")
    assert out == [
        "> assert(f(12, n) == 5)",
        "  assert(32 == 5)",
        "> subexpressions:",
        "  20 = n",
        "  32 = f(12, 20)",
    ]


def test_subexpression_string_repr():
    """
    tests "string pointers" are identified and their repr use %s.
    also tests that NULL is *not* identified as a string pointer.
    """
    out = run_tester("void test(const char *s)", 'assert(strstr("hello world", s) == NULL);',
                     'test("world");', extra_test="#include <string.h>\n")
    assert out == [
        '> assert(strstr("hello world", s) == NULL)',
        '  assert("world" == (nil))',
        '> subexpressions:',
        '  "world" = s',
        '  "world" = strstr("hello world", "world")'
    ]


def test_subexpression_evaluated_once():
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

    out = run_tester("void test(int n)", 'assert(call_me_once(n) == n);',
                     'test(3);', extra_test=extra)
    assert out == [
        "> assert(call_me_once(n) == n)",
        "  assert(4 == 3)",
        "> subexpressions:",
        "  3 = n",
        "  4 = call_me_once(3)",
    ]


def test_subexpression_not_evaluated():
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

    out = run_tester("void test(int n)", 'assert(n == 5 && dont_call_me(n) == n);',
                     'test(42);', extra_test=extra)
    assert out == [
        "> assert(n == 5 && dont_call_me(n) == n)",
        "  assert(42 == 5)",
        "> subexpressions:",
        "  42 = n",
    ]
