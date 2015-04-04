#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <asm/xen/hypercall.h>
#include <asm/xen/interface.h>
#include <asm/xen/page.h>
#include <asm/pgtable_types.h>
#include <linux/memcontrol.h>

#include <xen/amaro.h>

#define MAX_HOT_MFN 262144
#define NUM_SHARED_PAGES 256

void *vaddrs[NUM_SHARED_PAGES];
static unsigned long mfns[NUM_SHARED_PAGES];
static xen_pfn_t frame_list[MAX_HOT_MFN];
static int hsm_init_flag;

static int alloc_shared_pages(void);
static void free_shared_pages(void);

asmlinkage long sys_hsm_read(unsigned int start)
{
    unsigned int max_frames, frames_ppage, pidx;
    long fidx;
    unsigned long offset;
    struct frame *f;
    void *curr_base_vaddr;

    frames_ppage = PAGE_SIZE / sizeof(struct frame);
    max_frames = NUM_SHARED_PAGES * frames_ppage;
    offset = 0;
    start = start % max_frames;
    fidx = 0;
  
    printk("start=%u, frames_ppage=%u\n", start, frames_ppage);

    for(pidx = (start * NUM_SHARED_PAGES) / max_frames; pidx < NUM_SHARED_PAGES; ++pidx)
    {
        printk("pidx=%u\n", pidx);
        curr_base_vaddr = vaddrs[pidx];

        for(fidx = start - (frames_ppage * pidx); fidx < frames_ppage; ++fidx)
        {
            offset = fidx * sizeof(struct frame);
            f = (void *)(((unsigned long)curr_base_vaddr) + offset);

            if (f->mfn == 0)
            {
                printk("pidx=%u fidx=%ld mfn=0 break\n", pidx, fidx);
                return fidx + (frames_ppage * pidx);
            }
            else
            {
                printk("pidx=%u fidx=%ld mfn=%u\n", pidx, fidx, f->mfn);
            }
        }
    }

    return fidx + (frames_ppage * pidx);
}

static int alloc_shared_pages(void)
{
    long ret;
    unsigned int pidx;

    for (pidx = 0; pidx < NUM_SHARED_PAGES; ++pidx)
    {
        printk("allocating pidx=%u\n", pidx);

        if ((ret = HYPERVISOR_hsm_get_mfn(&mfns[pidx])) < 0)
        {
            printk("ERROR, hsm_get_mfn() returned: %ld\n", ret);
            return 1;
        }

        vaddrs[pidx] = kmalloc(PAGE_SIZE, GFP_KERNEL);
        set_pte_vaddr((unsigned long) vaddrs[pidx],
                      mfn_pte(mfns[pidx], PAGE_KERNEL));
    }
    
    printk("allocation ended\n");
    return 0;
}

asmlinkage long sys_hsm_alloc(void)
{
    printk("sys_hsm_alloc\n");
    return alloc_shared_pages();
}

static void free_shared_pages(void)
{
    int rc;
    unsigned int pidx;

    for (pidx = 0; pidx < NUM_SHARED_PAGES; ++pidx)
    {
        kfree(vaddrs[pidx]);
    }
    
    rc = HYPERVISOR_hsm_free_mfn((uint64_t) 0); // number is ignored

    if (rc != 0) {
        printk("ERROR, hsm_free_page() returned: %d\n", rc);
    }
}

asmlinkage long sys_hsm_free(void)
{
    printk("sys_hsm_free()");
    free_shared_pages();
    return 0;
}

xen_pfn_t *get_hotpage_list_sharedmem(unsigned int *hotcnt)
{
    unsigned int frames_ppage, pidx, fidx;
    unsigned long offset;
    struct frame *f;
    void *curr_base_vaddr;
    //unsigned long *lock = (unsigned long *)vaddrs[0];
	

	/* if not intialized, then initialize now*/
	if(!hsm_init_flag) {
		hsm_init_flag = 1;
		sys_hsm_alloc();
	}

    frames_ppage = PAGE_SIZE / sizeof(struct frame);
    offset = 0;

    //bit_spin_lock(0, lock);
  
	*hotcnt = 0;

    for(pidx = 0; pidx < NUM_SHARED_PAGES; ++pidx)
    {
        curr_base_vaddr = vaddrs[pidx];

        for(fidx = 0; fidx < frames_ppage; ++fidx)
        {
            //if (unlikely(pidx == 0 && fidx == 0))
              //  continue;

            offset = fidx * sizeof(struct frame);
            f = (void *)(((unsigned long)curr_base_vaddr) + offset);

            if (f->mfn == 0) {
                //*hotcnt = pidx * frames_ppage + fidx;
                //printk("hotcnt = %u fidx=%u pidx=%u "
				//		"frames_ppage=%u\n", *hotcnt, fidx, pidx, frames_ppage);
                return frame_list;
            }
			frame_list[pidx * frames_ppage + fidx] = f->mfn;
			*hotcnt = *hotcnt +1;
        }
    }

	if(MAX_HOT_MFN < *hotcnt)
	    *hotcnt = MAX_HOT_MFN;

    //bit_spin_unlock(0, lock);
    return frame_list;
}
