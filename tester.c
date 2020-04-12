#include <stdio.h>

int test_func(int n, int m);

static void call_test(int n, int m) {
    printf("calling test_func(%d, %d)...\n", n, m);
    test_func(n, m);
    printf("passed!\n");
}

int main(void) {
    call_test(5, 2);
    call_test(6, 5);
    return 0;
}
