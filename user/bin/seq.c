/* seq - print a sequence of numbers.  seq [first] last */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    long first = 1, last;
    if (argc == 2) {
        last = atol(argv[1]);
    } else if (argc == 3) {
        first = atol(argv[1]);
        last = atol(argv[2]);
    } else {
        fprintf(stderr, "usage: seq [first] last\n");
        return 2;
    }
    for (long i = first; i <= last; i++)
        printf("%ld\n", i);
    return 0;
}
