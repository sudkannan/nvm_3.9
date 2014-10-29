#include <linux/kernel.h>
#include <linux/syscalls.h>

asmlinkage long sys_hello(void)
{
    printk("Hello World sys_hello\n");
    return 0;
}
