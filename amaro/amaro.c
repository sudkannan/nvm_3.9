#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <asm/xen/hypercall.h>
#include <asm/xen/interface.h>
#include <asm/xen/page.h>
#include <asm/pgtable_types.h>

void *vaddr;
unsigned long mfn;

struct data {
    unsigned int first;
    unsigned int second;
    unsigned int third;
};

void print_page(void);

void print_page(void)
{
    char *tmp = vaddr;
    struct data d;
    memset((void *) &d, 0, sizeof(struct data));

    printk("DEBUG: loc %s %d \n", __FUNCTION__, __LINE__);

    memcpy((void *) &d, (void *) vaddr, sizeof(struct data));

    printk("first = %u\n", d.first);
    printk("second = %u\n", d.second);
    printk("third = %u\n", d.third);
}

asmlinkage long sys_hsm_alloc(void)
{
    int rc;

    printk("sys_hsm_alloc\n");

    rc = HYPERVISOR_hsm_get_mfn(&mfn);

    if (rc != 0) {
        printk("ERROR, hsm_get_page() returned: %d\n", rc);
        return rc;
    }

    printk("mfn = %lu\n", mfn);

    vaddr = kmalloc(PAGE_SIZE, GFP_KERNEL);
    set_pte_vaddr((unsigned long) vaddr, mfn_pte(mfn, PAGE_KERNEL));

    print_page();
    return 0;
}

asmlinkage long sys_hsm_free(void)
{
    int rc;
    printk("hsm_free() mfn = %lu\n", (unsigned long) mfn);

    kfree(vaddr);

    rc = HYPERVISOR_hsm_free_mfn((uint64_t) mfn);

    if (rc != 0) {
        printk("ERROR, hsm_free_page() returned: %d\n", rc);
        return rc;
    }

    vaddr = NULL;
    mfn = 0;
    return 0;
}
