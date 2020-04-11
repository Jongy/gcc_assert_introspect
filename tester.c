#include <stdio.h>

int test_func(int n);

static void call_test(int n) {
    printf("calling test_func(%d)\n", n);
    test_func(n);
    printf("passed!\n");
}

int main(void) {
    call_test(5);
    call_test(6);
    return 0;
}
