#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <asm/xen/hypercall.h>

asmlinkage long sys_hello(void)
{
    printk("Hello World sys_hello\n");
    HYPERVISOR_hypertest(0);
    printk("hypercall executed\n");
    return 0;
}
