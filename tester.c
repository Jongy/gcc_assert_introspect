#include <stdio.h>

int test_func(int n);

int main(void) {
    printf("calling test_func(5)\n");
    test_func(5);
    printf("calling test_func(6)\n");
    test_func(6);
    return 0;
}
