#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s <phys_addr> <offset>\n", argv[0]);
        return 0;
    }

    off_t offset = atoi(argv[1]);  /* phys_addr */
    size_t len = atoi(argv[2]);    /* offset */

    int fd = open("/dev/bigcpm", O_RDWR | O_SYNC);
    unsigned char *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
    if (mem == NULL) {
        printf("Can't map memory\n");
        return -1;
    }

    int i;
    for (i = 0; i < len; ++i) {
        	printf("%02x ", (int)mem[i]);
	}
}
