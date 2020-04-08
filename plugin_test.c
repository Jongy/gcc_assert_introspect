#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define RED(s) "\x1b[31m" s "\x1b[0m"

int func2(int n) {
    static int times_called;
    times_called++;
    if (times_called >= 2) {
        printf(RED("PLUGIN ERROR") " func2 evaluated more than once!\n");
        abort();
    }
    return n + 3;
}

int test_func(int n) {
    assert(1 != n && n != 6 || n == 12 || !n || func2(n) > 43879);
}
