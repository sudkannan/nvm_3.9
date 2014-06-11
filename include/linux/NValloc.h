#ifndef _LINUX_NVALLOC_H_
#define _LINUX_NVALLOC_H_

#include <linux/slab.h>
#include <linux/backing-dev.h>
#include <linux/mm.h>
#include <linux/shm.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/syscalls.h>
#include <linux/capability.h>
#include <linux/init.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/personality.h>
#include <linux/security.h>
#include <linux/hugetlb.h>
#include <linux/profile.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/mempolicy.h>
#include <linux/rmap.h>
#include <linux/mmu_notifier.h>
#include <linux/perf_event.h>
#include <linux/audit.h>
#include <asm/uaccess.h>
#include <asm/cacheflush.h>
#include <asm/tlb.h>
#include <asm/mmu_context.h>

#include <linux/linkage.h>
#include <linux/kernel.h>
#include <asm/uaccess.h>
#include <linux/mempool.h>

#define MAX_POOLS 1

//Currently restrict the write to less than one page
#define MAX_NV_WRITE_SIZE  4096

#define POOL_SIZE 1


#define MAX_PSTATE 1
#define PSTATE_NAME_LEN 128


//Create our own paging structure
//as of now just the page table
long  pram_page_tbl[PAGE_SIZE];

long page_counter = 0;
int slab_intialized = 0;


struct mem_pools {

     const char  *name;
     mempool_t *pram_mem_pool;
     struct kmem_cache *pram_slab;
     int size;
} pool_array[MAX_POOLS] = { { (char  *) "pram_size_128", NULL, NULL,128}};/*,
                                      { (char  *)"pram_size_256", NULL, 256},
                                      { (char  *) "pram_size_512", NULL, 512}, 
                                      { (char  *)"pram_size_1024", NULL, 1024}};*/

struct pstate {
        int next_nid_to_alloc;
        int next_nid_to_free;
        unsigned int order;
        unsigned long mask;
        unsigned long max_nv_pages;
        unsigned long nr_nv_pages;
        unsigned long free_nv_pages;
        unsigned long resv_nv_pages;
        unsigned long surplus_nv_pages;
        unsigned long nr_overcommit_nv_pages;
        struct list_head nv_freelists[MAX_NUMNODES];
        unsigned int nr_nv_pages_node[MAX_NUMNODES];
        unsigned int free_nv_pages_node[MAX_NUMNODES];
        unsigned int surplus_nv_pages_node[MAX_NUMNODES];
        char name[PSTATE_NAME_LEN];
};


struct pstate pstates[MAX_PSTATE];

//static struct page *nv_alloc_fresh_page_node(struct pstate *h, int nid, unsigned long size);

//static struct page *alloc_fresh_nv_page(struct pstate *h, nodemask_t *nodes_allowed);


//create a new cache slab
int create_slab_caches(void);

int initialize();

void* AllocateMemory( int len , unsigned long *addr  ); 


void* pool_page_alloc( gfp_t gfp_mask, void *pool_data );

void* pool_page_free( gfp_t gfp_mask, void *pool_data );

//create memory pool for PRAM allocation. 
////Need to be put somewhere in generic location
int create_memory_pool (void );


#endif


        
