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

int func3(int n) {
    printf(RED("PLUGIN ERROR") " func3 was evaluated!\n");
    abort();
}

int test_func(int n, int m) {
    assert((1 != n && n != 6 && n != 5 && func3(n)) || n == 5 || n == 12 || !n || func2(n) > 43879 || n * 4 == 54 + n || n / 5 == 10 - n || m == 93);
    return 5;
}
