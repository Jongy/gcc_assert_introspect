Assert Introspection
====================

(WIP) GCC plugin that creates ``pytest``-like assert introspection for C/C++, without
any modifications required.

TL;DR
-----

When ``assert(1 != n && n != 6 || n == 12 || !n || n > 43879)`` fails, you get this print before aborting::

    > assert(1 != n && n != 6 || n == 12 || !n || n > 43879)
      assert((((6 != 1) && (6 != 6)) || ((6 == 12) || (6 == 0))) || (6 > 43879))

Why
---

I always preferred the short and concise ``assert(...)`` statements (as opposed to the cumbersome
assert-equal, assert-less-than, assert-string-equal etc most C/C++ unit test libraries have).
And not just for writing tests - ``assert`` s placed in regular code are very helpful to catch
problems, and in many projects I use them extensively. However, often seeing them trigger
is just not enough to pinpoint the problem.
So you are required to change the code, add some prints and reproduce the problem if you really
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

*This was developed & tested with GCC 9.1.0 / 7.5. Actually latest commits broke GCC 7.5 support, this needs to be fixed*

Current PoC can be run with ``make test``. It compiles the plugin itself, then (with the plugin
active) compiles a short file containing a simple function, then compiles another file (without
the plugin this time) which calls that simple function.

The simple function is defined as follows::

    int test_func(int n) {
        assert(1 != n && n != 6 || n == 12 || !n || n > 43879);
    }

The test first calls it with ``5`` and we see the ``assert`` passes and nothing happens.
Then it's called again with ``6``, this time the ``assert`` triggers and the program aborts.
But since the plugin rewrote the assert, we get a much nicer print right before aborting::

    > assert(1 != n && n != 6 || n == 12 || !n || n > 43879)
      assert((((6 != 1) && (6 != 6)) || ((6 == 12) || (6 == 0))) || (6 > 43879))

Hooray :)

TODOs
-----

* Include all relevant "fields" from the original assert - ``__LINE__``, function name etc.
* Point at the specific subexpression that failed.
* Relate variable values to their names.
* Relate subexpression strings to values (function calls to their return values used in expression).
  This will probably require to "recreate" the code of the function call from AST.
* Show values of expressions inside function calls (for ``assert(f(n))`` show ``n`` as well)
* Get rid of redundant parenthesis.
* Write some tests to see it covers most
* Test it on some real projects :D
* Make it generic - not tied to glibc's ``assert``.
* Subtraction of consts is represented by ``PLUS_EXPR`` with a negative ``INTEGER_CST``, handle
  it nicely.

See the plugin code for more information.
