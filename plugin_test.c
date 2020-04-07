#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int test_func(int n) {
    assert(1 != n && n != 6 || n == 12 || !n || n > 43879);
}
