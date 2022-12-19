#include <stdio.h>
#include <sys/resource.h>

int main() {
    struct rlimit lim;
    char *errorMessage = "Ran into error during getrlimit operation.";

    // Change rlim_cur to rlim_max for hard limits
    if (getrlimit(RLIMIT_STACK, &lim) != 0) {
        printf("%s", errorMessage);
        return -1;
    }
    printf("stack size: %ld\n", lim.rlim_cur);

    if (getrlimit(RLIMIT_NPROC, &lim) != 0) {
        printf("%s", errorMessage);
        return -1;
    }
    printf("process limit: %ld\n", lim.rlim_cur);

    if (getrlimit(RLIMIT_NOFILE, &lim) != 0) {
        printf("%s", errorMessage);
        return -1;
    }
    printf("max file descriptors: %ld\n", lim.rlim_cur);

    return 0;
}