#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <asm/xen/hypercall.h>
#include <asm/xen/interface.h>
#include <asm/xen/page.h>
#include <asm/pgtable_types.h>

#define NUM_PAGES 4
void *vaddrs[NUM_PAGES];
static unsigned long mfns[NUM_PAGES];

struct frame {
    unsigned int mfn;
};

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
    max_frames = NUM_PAGES * frames_ppage;
    offset = 0;
    start = start % max_frames;
    fidx = 0;
  
    printk("start=%u, frames_ppage=%u\n", start, frames_ppage);

    for(pidx = (start * NUM_PAGES) / max_frames; pidx < NUM_PAGES; ++pidx)
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

    for (pidx = 0; pidx < NUM_PAGES; ++pidx)
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

    for (pidx = 0; pidx < NUM_PAGES; ++pidx)
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
