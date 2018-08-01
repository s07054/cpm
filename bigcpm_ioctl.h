#ifndef BIGCPM_IOCTL_H
#define BIGCPM_IOCTL_H
#include <linux/ioctl.h>

typedef struct
{
    unsigned long paddr;                      /* out: physical address */
    unsigned long size;                       /* in: Memory size */
} bigcpm_arg_t;
 
#define  BIGCPM_ALLOC		_IOW('b', 1, unsigned long)
#define  BIGCMP_RELEASE 	_IO('b',  2)
#define  BIGCMP_GET_PHYSADDR  	_IOR('b', 3, unsigned long )
 
#endif
