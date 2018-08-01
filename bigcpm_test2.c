#include <stdio.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
 
#include "bigcpm_ioctl.h"

typedef unsigned long phys_addr_t;

typedef struct dma_addr{
        phys_addr_t phy;
        void * virt;
}dma_addr_t;

#define ALLOC_SIZE (600*1024*1024)

static dma_addr_t base_addr={0};

int dma_cpm_mmap_buf(int fd, int alloc_size, void **pvirt )
{
    alloc_size += (getpagesize() - 1);
    alloc_size &= ~(getpagesize() - 1);

    if ((alloc_size % getpagesize()) != 0 )
    {
                close(fd);
                printf("arguments not page-aligned: "
                        "length 0x%x ", alloc_size);
                        return -1;
     }

     (*pvirt) = mmap(0, alloc_size,
                        PROT_READ|PROT_WRITE, MAP_SHARED|MAP_LOCKED, fd, 0);

     if ((*pvirt) == MAP_FAILED) {
                printf("mmap failed for size %d: %s\n",
                                alloc_size, strerror(errno));
                close(fd);
                return -1;
      }
	printf("Got pvirt from mmap 0x%x\n",*pvirt);
	return 0;
}

/* given phys address and get virtual address */
void test_mmap(int fd)
{
     int i, ret, *pvirt; 

     int alloc_size=ALLOC_SIZE;

     int fd_m = open("/dev/mem", O_RDWR|O_SYNC);
     
     if(base_addr.phy) 
	{
     		base_addr.virt = mmap(NULL, 
				alloc_size, 
				PROT_READ | PROT_WRITE, MAP_SHARED, 
				fd_m, 
				base_addr.phy);

		pvirt=base_addr.virt;
		printf("test write with virt base address 0x%x\n",pvirt);
     		for(i=0; i< 10; i++) 
		{
			*pvirt = i; pvirt++;
		} 	
		pvirt=base_addr.virt;
		printf("test read with virt base address 0x%x\n",pvirt);
     		/* test read */
     		for(i=0; i< 10; i++) 
		{
			printf("read back 0x%x\n", *pvirt);
			pvirt++;
		} 	

		ret = munmap(base_addr.virt, alloc_size);
		if(ret == -1 ) 
		{
        		printf("munmap failed with%s\n", strerror(errno));
		}	 	
	}else{
		printf(" ERROR base_addr.phy is 0\n");
	}
	close(fd_m);
}
 
void get_addr(int fd)
{
    bigcpm_arg_t b;
 
    if (ioctl(fd, BIGCMP_GET_PHYSADDR, &b) == -1)
    {
        printf("BIGCMP_GET_PHYSADDR failed: %s\n", strerror(errno));
    }
    else
    {	
	base_addr.phy=b.paddr;
        printf("phys addr : 0x%x\n", base_addr.phy);
    }
}
void alloc(int fd)
{
    bigcpm_arg_t q;
    q.size = ALLOC_SIZE;

    if (ioctl(fd, BIGCPM_ALLOC, &q) == -1)
    {
        printf("BIGCPM_ALLOC, failed: %s\n", strerror(errno));
    }
}
void release(int fd)
{
    if (ioctl(fd, BIGCMP_RELEASE) == -1)
    {
        printf("BIGCMP_RELEASE, failed: %s\n", strerror(errno));
    }
}
 
int main(int argc, char *argv[])
{
    char *file_name = "/dev/bigcpm";
    int fd;
    char *cvalue = NULL;
    int c;
    opterr = 0;

    while ((c = getopt (argc, argv, "abc:")) != -1)
         switch (c)
           {
           case 'a':
             break;
           case 'b':
             break;
           case 'c':
             cvalue = optarg;
             break;
           case '?':
             if (optopt == 'c')
               fprintf (stderr, "Option -%c requires an argument.\n", optopt);
             else if (isprint (optopt))
               fprintf (stderr, "Unknown option `-%c'.\n", optopt);
             else
               fprintf (stderr,
                        "Unknown option character `\\x%x'.\n",
                        optopt);
             return 1;
           default:
             abort ();
           }
    fd = open(file_name, O_RDWR);
    if (fd == -1)
    {
        perror("apps open");
        return 2;
    }
 
    alloc(fd);
    get_addr(fd);
    test_mmap(fd);
    release(fd);
 
    close (fd);
 
    return 0;
}
