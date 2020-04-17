Assert Introspection
====================

(WIP) GCC plugin that creates ``pytest``-like assert introspection for C (and possibly C++),
without any code modifications required.

TL;DR
-----

When ``assert(1 != n && n != 6)`` fails, you get this print before aborting::

    In file myfile.c:42, function 'test_function':
    > assert(1 != n && n != 6)
      assert((...) && 6 != 6)
    > subexpressions:
      6 = n

Function calls and strings inside the ``assert`` are also displayed nicely::

    // s is "42";

    > assert(strtol(s, NULL, 0) == 5)
      assert(42 == 5)
    > subexpressions:
      "42" = s
      42 = strtol("42", (nil), 0)

Why
---

I always preferred the short and concise ``assert(...)`` statements (as opposed to the cumbersome
assert-equal, assert-less-than, assert-string-equal etc most C/C++ unit test libraries have).
And not just for writing tests - ``assert`` s placed in regular code are very helpful to catch
problems, and in many projects I use them extensively, especially during early development,
when they are very likely to fail... :) However, often seeing them trigger is just not enough to
pinpoint the problem.
So you are required to change the code, add some prints and reproduce the problem if you
want to know what went wrong.

I'm really tired of doing that (especially of converting ``assert`` s which have side effects to
equivalent ``printf`` s, then having to convert them back when the problem is solved...)

``pytest`` solved it nicely with their assert introspection. When an introspected ``assert`` fails
in ``pytest``, it prints the values of all sub-expressions in the main assert expression. For
example, when the following ``assert`` fails::

    assert min([1, 2, 3]) - 5 + min(1, 2, 3) == max([5, 6, 5])

``pytest`` prints::

    >       assert min([1, 2, 3]) - 5 + min(1, 2, 3) == max([5, 6, 5])
    E       assert ((1 - 5) + 1) == 6
    E        +  where 1 = min([1, 2, 3])
    E        +  and   1 = min(1, 2, 3)
    E        +  and   6 = max([5, 6, 5])

Very neat. I want that in C.

How
---

``pytest`` does that by rewriting Python's AST (see a brief covering of it here_). This way, the
asserted expression can be written naturally by the user, and after parsing into AST it can be
rewritten they please to add the extra information.

.. _here: http://pybites.blogspot.com/2011/07/behind-scenes-of-pytests-new-assertion.html

In my case, since I want the expressions to be written naturally in C, we'll have to do something
similar (so we get parsed C expressions).
C is not a dynamic language like Python, so the AST can't be patched in runtime, it must be changed
during compilation. This can be done by writing a GCC plugin that'll patch the AST during
compilation.

Current PoC
-----------

*This was developed & tested with GCC 9.1.0 / 7.5.0*

Current PoC can be run with ``make test``. It compiles the plugin itself, then (with the plugin
active) compiles a short file containing a simple function, then compiles another file (without
the plugin this time) which calls that simple function.

The simple function is defined as follows::

    int test_func(int n, int m) {
        assert((1 != n && n != 6 && n != 5 && func3(n)) || n == 5 || n == 12 || !n || func2(n) > 43879 || n * 4 == 54 + n || n / 5 == 10 - n);
    }

The test first calls it with ``5, 2`` and we see the ``assert`` passes and nothing happens.
Then it's called again with ``6, 5``, this time the ``assert`` triggers and the program aborts.
But since the plugin rewrote the assert, we get a much nicer print right before aborting::

    In plugin_test.c:33, function 'test_func':
    > assert((1 != n && n != 6 && n != 5 && func3(n)) || n == 5 || n == 12 || !n || func2(n) > 43879 || n * 4 == 54 + n || n / 5 == 10 - n || m == 93)
      assert((((((((((...) && (6 != 6)))) || ((6 == 5) || (6 == 12))) || (6 == 0)) || (9 > 43879)) || (6 * 4 == 6 + 54)) || (6 / 5 == 10 - 6)) || (5 == 93))
    > subexpressions:
      6 = n
      5 = m
      9 = func2(6)


Hooray :)

Tests
-----

Run with ``make test``. They'll compile some test programs and check their output. You
can use it to verify your local GCC is okay with the plugin.

TODOs
-----

* Relate subexpression strings to values. We already relate variables and results of function calls,
  others might be useful as well (for example, results of arithmetics?)
* Get rid of redundant parenthesis.
* Write some tests to see it covers most
* Test it on some real projects :D
* Make it generic - not tied to glibc's ``assert``.
* Subtraction of consts is represented by ``PLUS_EXPR`` with a negative ``INTEGER_CST``, handle
  it nicely.
* Casts are displayed on variables, but not on function calls / binary expression results.

See the plugin code for more information.
