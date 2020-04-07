#include <stdio.h>

int test_func(int n);

int main(void) {
    printf("calling test_func(1)\n");
    test_func(1);
    printf("calling test_func(42)\n");
    test_func(42);
    return 0;
}
