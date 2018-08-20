#include <linux/init.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <asm/uaccess.h>
#include <asm/io.h>

#include "debug_trace.h"
#include "bigcpm_ioctl.h"

#define FIRST_MINOR 0
#define MINOR_CNT 2

/* local variable */ 
static dev_t dev;
static struct cdev c_dev;
static struct class *cl;

static int allocated=0;

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("BIG CPM Char Driver");

#ifdef DEBUG
#define TRACEF(fmt, args...)	printk(KERN_DEBUG fmt, ## args)
#else
#define TRACEF(fmt, args...)
#endif

/* Chapter: basic allocation unit retrieved via the buddy allocator */

#define CHAPTER_ORDER (MAX_ORDER - 1)	 /* page order of chapter */
#define CHAPTER_PAGES (1 << CHAPTER_ORDER)	 /* pages in a chapter */
#define CHAPTER_SIZE (PAGE_SIZE * CHAPTER_PAGES) /* chapter size in bytes */


/*
* We join adjacent chapters into clusters, keeping track of allocations
* as an ordered set of clusters.
*
* Note that the physical page frame number (pfn) is stored in the hopes
* that continuous pfn's represent continuous memory. Should we merge
* clusters via dma_addr_t physical addresses?
*/

struct cluster {
struct list_head head;	/* ordered list of clusters */
ulong page_first;	/* first page in cluster */
ulong page_count;	/* number of pages */
};

struct cluster_set {
struct list_head clusters;	/* allocated clusters */
};

/* Declare and initialize a cluster set. */
#define CLUSTER_SET(name) \
struct cluster_set name = { clusters: LIST_HEAD_INIT(name.clusters) }

/* Retrieve the cluster from it's list head. */
static struct cluster *get_cluster(struct list_head *node)
{
return list_entry(node, struct cluster, head);
}

static inline dma_addr_t phys_start(const struct cluster *cl)
{
return page_to_phys(pfn_to_page(cl->page_first));
}

static inline dma_addr_t phys_end(const struct cluster *cl)
{
return page_to_phys(nth_page(pfn_to_page(cl->page_first), cl->page_count));
}

/* Return node after target location for new chapter (passed as pfn). */
static struct list_head *find_insert_location(
		struct cluster_set *set, ulong chapter_start)
{
	struct list_head *p;
	list_for_each (p, &set->clusters) {
	if (get_cluster(p)->page_first > chapter_start)
		return p;
		}
	return &set->clusters;
}

/* Try to merge a new chapter by prepending it to the cluster at pos.
Return true on success, false if unable to merge. */
static bool try_prepend(
struct cluster_set *set, struct list_head *pos, ulong chapter_start)
{
	if (pos != &set->clusters) {
		struct cluster *cl = get_cluster(pos);
		if (chapter_start + CHAPTER_PAGES == cl->page_first) {
			cl->page_first = chapter_start;
			cl->page_count += CHAPTER_PAGES;
		return true;
		}
	}
	return false;
}

/* Try to merge a new chapter by appending it to cluster at pos.
Return true on success, false if unable to merge. */
static bool try_append(
struct cluster_set *set, struct list_head *pos, ulong chapter_start)
{
	if (pos != &set->clusters) {
		struct cluster *cl = get_cluster(pos);
		if (cl->page_first + cl->page_count == chapter_start) {
			cl->page_count += CHAPTER_PAGES;
		return true;
		}
	}
	return false;
}

/* Tries to merge the cluster previous to pos into it.
Returns true on success (invalidates previous cluster, pos stays valid). */
static void try_merge_prev(struct cluster_set *set, struct cluster *pos)
{
	struct list_head *prev_head = pos->head.prev;

	if (prev_head != &set->clusters) {
		struct cluster *prev = get_cluster(prev_head);
	if (prev->page_first + prev->page_count == pos->page_first) {
		pos->page_first = prev->page_first;
		pos->page_count += prev->page_count;
		list_del(prev_head);
		kfree(prev);
		}
	}
}

/* Account for another chapter allocation, returning the cluster it became
part of. Returns NULL on error (out of memory). */
static struct cluster *add_alloc(struct cluster_set *set,
				struct page *new_chapter)
{
	ulong chapter_start = page_to_pfn(new_chapter);

	struct list_head *insert_loc = find_insert_location(set, chapter_start);
	if (try_prepend(set, insert_loc, chapter_start)) {
	struct cluster *cl = get_cluster(insert_loc);
	try_merge_prev(set, cl);
	return cl;
	} else if (try_append(set, insert_loc->prev, chapter_start)) {
	return get_cluster(insert_loc->prev);
	} else {
	struct cluster *new_cluster = kmalloc(GFP_KERNEL, sizeof(*new_cluster));
	if (new_cluster) {
		new_cluster->page_first = chapter_start;
		new_cluster->page_count = CHAPTER_PAGES;
		list_add_tail(&new_cluster->head, insert_loc);
	}
	return new_cluster;
	}
}

/* Give up count chapters starting at start. */
static void free_chapters(struct page *start, unsigned long count)
{
	unsigned long i;
	TRACEF("Freeing %lu chapters @ 0x%lx.\n", count, (ulong) page_to_phys(start));
	for (i = 0; i < count; i++, start = nth_page(start, CHAPTER_PAGES)) {
		__free_pages(start, CHAPTER_ORDER);
	}
}

/* Free the set and all clusters allocated to it. */
static void free_set(struct cluster_set *set)
{
	struct cluster *pos, *t;
	TRACEF("Freeing clusters.\n");
	list_for_each_entry_safe(pos, t, &set->clusters, head) {
	free_chapters(pfn_to_page(pos->page_first), pos->page_count / CHAPTER_PAGES);
	kfree(pos);
}
}

/* Lists the allocations in the given cluster set. */
static void list_allocs(struct cluster_set *set)
{
	struct cluster *cluster;
	TRACEF("Allocations in ascending order:\n");

	list_for_each_entry(cluster, &set->clusters, head) {
		TRACEF("Cluster from 0x%08lx .. 0x%08lx (%lu pages).\n",
		(ulong) phys_start(cluster),
		(ulong) phys_end(cluster),
		cluster->page_count);
	}
}

/* Remove the given cluster from the set, keeping the allocation. */
static void unlink_cluster(struct cluster_set *set, struct cluster *cl)
{
	list_del_init(&cl->head);
}

/* Allocate a big buffer of given size [bytes]. flags as in alloc_pages. */
struct page *bigbuf_alloc(unsigned int flags, ulong size)
{
	int order = size ? get_order(size) : 0;

	if (order <= CHAPTER_ORDER) {
		return alloc_pages(flags, order);
	} else {
		struct page *result;

		int chapters = (size + CHAPTER_SIZE - 1) / CHAPTER_SIZE;
		CLUSTER_SET(allocation_set);
		struct cluster *merged;

		TRACEF("Allocate huge block of size %lu (%i chapters).\n", size, chapters);

		do {
			struct page *chapter = alloc_pages(flags, CHAPTER_ORDER);
			if (!chapter)
			goto fail;
			TRACEF("Allocated chapter @ %lx.\n",
			(ulong) page_to_phys(chapter));
			merged = add_alloc(&allocation_set, chapter);
			if (!merged)
				goto fail;
			list_allocs(&allocation_set);
		} while (merged->page_count < chapters * CHAPTER_PAGES);

		unlink_cluster(&allocation_set, merged);
		TRACEF("After taking result:\n");
		list_allocs(&allocation_set);
		free_set(&allocation_set);
		result = pfn_to_page(merged->page_first);
		kfree(merged);
		return result;
	fail:
		free_set(&allocation_set);
		TRACEF("Allocation failed.\n");
		return NULL;
	} /* else */
}

/* Free a buffer allocates by bigbuf_alloc. */
void bigbuf_free(struct page *start, ulong size)
{
	int size_order = size ? get_order(size) : 0;
	if (size_order < CHAPTER_ORDER) {
		__free_pages(start, size_order);
	} else {
	free_chapters(start, (size + CHAPTER_SIZE - 1) / CHAPTER_SIZE);
	}
}

typedef struct bigcpmdev_info {
  unsigned int   size;
  phys_addr_t    paddr;
  struct page    *huge_block;
} bigcpmdev_info_t;

/*local */
static struct bigcpmdev_info bigcpm_info;

static int bigcpm_open(struct inode *i, struct file *f)
{
    return 0;
}
static int bigcpm_close(struct inode *i, struct file *f)
{
    return 0;
}

static int alloc_bigcpm_dev (int size)
{
	bigcpm_info.huge_block = bigbuf_alloc(GFP_KERNEL | __GFP_HIGHMEM, size);
	if (bigcpm_info.huge_block) 
	{
		printk(KERN_INFO "Allocated block at 0x%lx.\n", (ulong) page_to_phys(bigcpm_info.huge_block));

		bigcpm_info.paddr=page_to_phys(bigcpm_info.huge_block);
	}else {
		printk(KERN_ERR "Allocation of size %u failed.\n", size);
		return -1;
	}
	return 0;
}


#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35))
static int bigcpm_ioctl(struct inode *i, struct file *f, unsigned int cmd, unsigned long arg)
#else
static long bigcpm_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
#endif
{
    bigcpm_arg_t q;
 
    switch (cmd)
    {
        case BIGCMP_GET_PHYSADDR:
	    /* be carefull that phys_addr_t can be 64 bits */
	    printk("BIGCMP_GET_PHYSADDR 0x%x \n", (unsigned long)bigcpm_info.paddr);
	    q.paddr= bigcpm_info.paddr;
	    q.size = bigcpm_info.size;
            if (copy_to_user((bigcpm_arg_t *)arg, &q, sizeof(bigcpm_arg_t)))
            {
	    	printk("BIGCMP_GET_PHYSADDR failed \n");
                return -EACCES;
            }
	    
            break;
        case BIGCMP_RELEASE:
   		if (bigcpm_info.huge_block) {
			printk(KERN_INFO "Freeing block at 0x%lx.\n",
					(ulong) page_to_phys(bigcpm_info.huge_block));
#if 1			
			if (allocated) {
				bigbuf_free(bigcpm_info.huge_block, bigcpm_info.size);
				allocated = 0;
			}
#endif
		}
            break;
        case BIGCPM_ALLOC:
            if (copy_from_user(&q, (bigcpm_arg_t *)arg, sizeof(bigcpm_arg_t)))
            {
	    	printk("BIGCPM_ALLOC failed \n");
                return -EACCES;
            }

	    bigcpm_info.size=q.size;
	    printk("BIGCPM_ALLOC:size 0x%x\n",q.size);
#if 1
	    if(!allocated) 
	    {	
	    	if (alloc_bigcpm_dev(q.size) != 0 )
	    	{
	    		printk("ERROR in alloc_bigcpm_dev()\n");
			return -EINVAL;
	    	}
		allocated = 1;
	    }
#endif
            break;
        default:
            return -EINVAL;
    }
 
    return 0;
}

/*
  * Common VMA ops.
  */
 
void bigcpm_vma_open(struct vm_area_struct *vma)
 {
         printk(KERN_NOTICE "bigcpm VMA open, virt %lx, phys %lx\n",
                         vma->vm_start, vma->vm_pgoff << PAGE_SHIFT);
 }
 
 void bigcpm_vma_close(struct vm_area_struct *vma)
 {
         printk(KERN_NOTICE "bigcpm VMA close.\n");
 }
 

/*
  * The remap_pfn_range version of mmap.  This one is heavily borrowed
  * from drivers/char/mem.c.
  */
 
static struct vm_operations_struct bigcpm_remap_vm_ops = {
         .open =  bigcpm_vma_open,
         .close = bigcpm_vma_close,
};

static int bigcpm_mmap(struct file *file, struct vm_area_struct *vma)
{
  	size_t size = vma->vm_end - vma->vm_start;
	if (!(vma->vm_flags & VM_SHARED)) 
	{
	   	pr_err("Mapping must be shared. Use MAP_SHARED flag in mmap! \n");
    		return -EINVAL;
  	}
	if ( ((vma->vm_pgoff << PAGE_SHIFT) + size ) > bigcpm_info.size) 
	{
    		pr_err ("%s: Attempting to Map more than MAX pgoff=%x, size=%x, bigcpm_size=%x\n",
      		__func__,
		(u32)vma->vm_pgoff, size, bigcpm_info.size);
    		return -EINVAL;
  	}
#if 0
        if (vma->vm_pgoff) {
                pr_err("%s: non-zero mmap page-offset 0x%lx is invalid\n",
                        __func__, vma->vm_pgoff);
                return -EINVAL;
        }
        if (size != bigcpm_info.size) {
                pr_err("%s: mmap size 0x%llx doesn't match region (0x%llx)\n",
                        __func__, (unsigned long long)size, bigcpm_info.size);
                return -EINVAL;
        }
        if (vma->vm_start & (bigcpm_info.size - 1)) {
                pr_err("%s: un-aligned mapping %llx:%llx -> %lx\n",
                        __func__, bigcpm_info.paddr, bigcpm_info.size,
                        vma->vm_start);
                return -EINVAL;
        }
	vma->vm_ops = &bigcpm_remap_vm_ops;
	bigcpm_vma_open(vma);
#endif
	TRACEF("vma->vm_pgoff 0x%x, size 0x%x\n",vma->vm_pgoff, size);

        if (remap_pfn_range(vma, 
			vma->vm_start, 
			(bigcpm_info.paddr+(vma->vm_pgoff<<PAGE_SHIFT))>> PAGE_SHIFT,
			size,
                        vma->vm_page_prot)
		)
	{
                return -EAGAIN;
	}
        return 0;
}

static struct file_operations bigcpm_fops =
{
    .owner = THIS_MODULE,
    .open = bigcpm_open,
    .release = bigcpm_close,
    .mmap     = bigcpm_mmap,
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35))
    .ioctl =bigcpm_ioctl
#else
    .unlocked_ioctl = bigcpm_ioctl
#endif
};

static int bigcpm_init(void)
{
    int ret;
    struct device *dev_ret;

	 
    if ((ret = alloc_chrdev_region(&dev, FIRST_MINOR, MINOR_CNT, "bigcpm_region")) < 0)
    {
        return ret;
    }
 
    cdev_init(&c_dev, &bigcpm_fops);
 
    if ((ret = cdev_add(&c_dev, dev, MINOR_CNT)) < 0)
    {
        return ret;
    }
     
    if (IS_ERR(cl = class_create(THIS_MODULE, "char")))
    {
        cdev_del(&c_dev);
        unregister_chrdev_region(dev, MINOR_CNT);
        return PTR_ERR(cl);
    }
    if (IS_ERR(dev_ret = device_create(cl, NULL, dev, NULL, "bigcpm")))
    {
        class_destroy(cl);
        cdev_del(&c_dev);
        unregister_chrdev_region(dev, MINOR_CNT);
        return PTR_ERR(dev_ret);
    }
 
	return 0;
}

static void bigcpm_exit(void)
{
   if ( allocated && bigcpm_info.huge_block) {
		printk(KERN_INFO "Freeing block at 0x%lx.\n",
			(ulong) page_to_phys(bigcpm_info.huge_block));
		bigbuf_free(bigcpm_info.huge_block, bigcpm_info.size);
	}
    device_destroy(cl, dev);
    class_destroy(cl);
    cdev_del(&c_dev);
    unregister_chrdev_region(dev, MINOR_CNT);

}

module_init(bigcpm_init);
module_exit(bigcpm_exit);
