#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fsyncfile.h"

int fsyncfile(int fd) {

    struct stat st;

    if (fstat(fd, &st) == -1) return -1;
    if (!S_ISREG(st.st_mode)) return 0;
    return fsync(fd);
}
