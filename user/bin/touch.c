/* touch - create empty files */
#include <stdio.h>
#include <fresh.h>

int main(int argc, char **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT);
        if (fd < 0) {
            fprintf(stderr, "touch: %s: %s\n", argv[i], strerror(fd));
            rc = 1;
            continue;
        }
        close(fd);
    }
    return rc;
}
