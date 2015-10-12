#include <linux/kernel_stat.h>
#include <linux/mm.h>
#include <linux/nvstruct.h>
#include <linux/hugetlb.h>
#include <linux/mman.h>
#include <linux/swap.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/ksm.h>
#include <linux/rmap.h>
#include <linux/module.h>
#include <linux/delayacct.h>
#include <linux/init.h>
#include <linux/writeback.h>
#include <linux/memcontrol.h>
#include <linux/mmu_notifier.h>
#include <linux/kallsyms.h>
#include <linux/swapops.h>
#include <linux/elf.h>
#include <linux/gfp.h>
#include <asm/io.h>
#include <asm/pgalloc.h>
#include <asm/uaccess.h>
#include <asm/tlb.h>
#include <asm/tlbflush.h>
#include <asm/pgtable.h>
#include <linux/rbtree.h>

#include <linux/linkage.h>
#include <linux/kernel.h>
#include <asm/uaccess.h>
#include <linux/NValloc.h>

#include <xen/xen-ops.h>
#include <xen/xen.h>
#include <xen/interface/xen.h>
#include <xen/features.h>
#include <xen/page.h>
#include <xen/xen-ops.h>
#include <xen/balloon.h>
#include <xen/heteromem.h>

#define BACKING_STORE

#ifdef BACKING_STORE
#include <linux/module.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/vfs.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/mount.h>
#include <linux/bitops.h>
#include <linux/cred.h>
#include <linux/backing-dev.h>

#include <linux/fs.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/vmstat.h>
#include <linux/sched.h>
#include <linux/math64.h>
#include <linux/writeback.h>
#include <linux/compaction.h>
#endif

//#define LOCAL_DEBUG_FLAG_1
//#define LOCAL_DEBUG_FLAG
//#define NV_STATS_DEBUG
//#define NVM_OPTIMIZE_1
//#define NVM_OPTIMIZE_2
//#define NVM_OPTIMIZE_3
//#ifdef NVM_OPTIMIZE_2
#define NVM_OPTIMIZE_2_3
#define BATCHSIZE 32*1024*1024
#define CHUNK_VMACACHESZ 256
#define NVM_JOURNAL
#define NVMJOURNALSZ 4*1024*1024
#define MAX_LOG_ENTERIES 4
//#endif

//Currently restrict the write to less than one page
#define MAX_NV_WRITE_SIZE  4096
/*MACROS for setting map address */
#define SETSLAB( addr, index ) ({  index = index << 24; addr = addr | index; } )  
#define for_each_pstate(p) \
          for ((p) = pstates; (p) < &pstates[MAX_PSTATE]; (h)++)


/*---Global variales--*/
unsigned int g_nv_init_pgs;
/*List containing all project id's */
struct list_head nv_proc_objlist;
int proc_list_init_flag;
#ifdef NV_STATS_DEBUG                                         
unsigned int g_used_nvpg;
unsigned int g_num_persist_pg;
unsigned int g_nvpg_read_cnt;
#endif

__cacheline_aligned_in_smp DEFINE_SPINLOCK(nv_proclist_lock);

/*---Global variales--*/
#ifdef NV_STATS_DEBUG
	void print_global_nvstats(void);
#endif

#ifdef BACKING_STORE
#define BACKINGFILE "/proc/nvmaps"
void nvm_load_from_file(char *nvm_backing_file);
#endif

#ifdef NVM_OPTIMIZE_2_3
void *nvpage_start = NULL;
unsigned int nvpage_used;
#endif

#ifdef NVM_OPTIMIZE_2
unsigned long chunkid_vma_cache[CHUNK_VMACACHESZ];
unsigned int vmacache_cnt;	
#endif

/*-------------static function list---------------------------------------*/
static int create_add_chunk(struct nv_proc_obj *proc_obj, 
							struct rqst_struct *rqst);
static int add_chunk(struct nv_chunk *chunk, struct nv_proc_obj *proc_obj);
struct nv_chunk* find_chunk( unsigned int vma_id, 
							struct nv_proc_obj *proc_obj );
struct nv_proc_obj* get_process_obj(struct nv_chunk *chunk);
static struct nv_chunk* create_chunk_obj(struct rqst_struct *rqst,
										 struct nv_proc_obj* proc_obj);
struct page* get_page_frm_chunk(struct nv_chunk *chunk, long pg_off);
unsigned int find_offset(struct vm_area_struct *vma, unsigned long addr);
struct nv_chunk* iterate_chunk(struct nv_proc_obj *proc_obj);
long sys_clear_all_persist( unsigned long proc_id);

/*************** NVM PERSISTENCE FUNCTIONS AND ATOMICS***************/
static inline void pvm_flush_buffer(void *buf, uint32_t len, bool fence)
{
    uint32_t i;
    len = len + ((unsigned long)(buf) & (CACHELINE_SIZE - 1));
    for (i = 0; i < len; i += CACHELINE_SIZE)
        asm volatile ("clflush %0\n" : "+m" (*(char *)(buf+i)));
    /* Do a fence only if asked. We often don't need to do a fence
     * immediately after clflush because even if we get context switched
     * between clflush and subsequent fence, the context switch operation
     * provides implicit fence. */
    if (fence)
        asm volatile ("sfence\n" : : );
}	

/* Provides ordering from all previous clflush too */
static inline void PERSISTENT_MARK(void)
{
    /* TODO: Fix me. */
}

static inline void PERSISTENT_BARRIER(void)
{
    asm volatile ("sfence\n" : : );
}


/* uses CPU instructions to atomically write up to 8 bytes */
static inline void nvm_memcpy_atomic (void *dst, const void *src, u8 size)
{
    switch (size) {
        case 1: {
            volatile u8 *daddr = dst;
            const u8 *saddr = src;
            *daddr = *saddr;
            break;
        }
        case 2: {
            volatile u16 *daddr = dst;
            const u16 *saddr = src;
            *daddr = cpu_to_le16(*saddr);
            break;
        }
        case 4: {
            volatile u32 *daddr = dst;
            const u32 *saddr = src;
            *daddr = cpu_to_le32(*saddr);
            break;
        }
        case 8: {
            volatile u64 *daddr = dst;
            const u64 *saddr = src;
            *daddr = cpu_to_le64(*saddr);
            break;
        }
        default:
            printk(KERN_ALERT "error: memcpy_atomic called with %d bytes\n", size);
            //BUG();
     }
}


/* assumes the length to be 4-byte aligned */
static inline void memset_nt(void *dest, uint32_t dword, size_t length)
{
    uint64_t dummy1, dummy2;
    uint64_t qword = ((uint64_t)dword << 32) | dword;

    asm volatile ("movl %%edx,%%ecx\n"
        "andl $63,%%edx\n"
        "shrl $6,%%ecx\n"
        "jz 9f\n"
        "1:      movnti %%rax,(%%rdi)\n"
        "2:      movnti %%rax,1*8(%%rdi)\n"
        "3:      movnti %%rax,2*8(%%rdi)\n"
        "4:      movnti %%rax,3*8(%%rdi)\n"
        "5:      movnti %%rax,4*8(%%rdi)\n"
        "8:      movnti %%rax,5*8(%%rdi)\n"
        "7:      movnti %%rax,6*8(%%rdi)\n"
        "8:      movnti %%rax,7*8(%%rdi)\n"
        "leaq 64(%%rdi),%%rdi\n"
        "decl %%ecx\n"
        "jnz 1b\n"
        "9:     movl %%edx,%%ecx\n"
        "andl $7,%%edx\n"
        "shrl $3,%%ecx\n"
        "jz 11f\n"
        "10:     movnti %%rax,(%%rdi)\n"
 	"leaq 8(%%rdi),%%rdi\n"
        "decl %%ecx\n"
        "jnz 10b\n"
        "11:     movl %%edx,%%ecx\n"
        "shrl $2,%%ecx\n"
        "jz 12f\n"
        "movnti %%eax,(%%rdi)\n"
        "12:\n"
        : "=D"(dummy1), "=d" (dummy2) : "D" (dest), "a" (qword), "d" (length) : "memory", "rcx");
}
/*************** NVM PERSISTENCE FUNCTIONS AND ATOMICS***************/



/**********TRANSACTION STRUCTURE HELPER*******/

uint16_t genid=0;
uint16_t next_transaction_id=0;

static inline uint32_t next_log_entry(uint32_t jsize, uint32_t le_off)
{
    le_off = le_off + LOGENTRY_SIZE;
    if (le_off >= jsize)
        le_off = 0;
    return le_off;
}

static inline uint32_t prev_log_entry(uint32_t jsize, uint32_t le_off)
{
    if (le_off == 0)
        le_off = jsize;
    le_off = le_off - LOGENTRY_SIZE;
    return le_off;
}

static inline uint16_t next_gen_id(uint16_t gen_id)
{
    gen_id++;
    /* check for wraparound */
    if (gen_id == 0)
        gen_id++;
    return gen_id;
}

static inline uint16_t prev_gen_id(uint16_t gen_id)
{
    gen_id--;
    /* check for wraparound */
    if (gen_id == 0)
        gen_id--;
    return gen_id;
}

/**********CODE RELATED TO INIT AND ALLOC OF TRANSACTION STRUCTURE*******/

static struct kmem_cache *pmfs_transaction_cachep;

static int __init init_transaction_cache(void)
{
    pmfs_transaction_cachep = kmem_cache_create("pmfs_journal_transaction",
            sizeof(pmfs_transaction_t), 0, (SLAB_RECLAIM_ACCOUNT |
            SLAB_MEM_SPREAD), NULL);
    if (pmfs_transaction_cachep == NULL) {
        printk(KERN_ALERT "PMFS: failed to init transaction cache\n");
        return -ENOMEM;
    }
    return 0;
}

static void destroy_transaction_cache(void)
{
    if (pmfs_transaction_cachep)
        kmem_cache_destroy(pmfs_transaction_cachep);
    pmfs_transaction_cachep = NULL;
}

inline pmfs_transaction_t *pvm_alloc_transaction(void)
{
    return (pmfs_transaction_t *)
        kmem_cache_alloc(pmfs_transaction_cachep, GFP_NOFS);
}

inline void pvm_free_transaction(pmfs_transaction_t *trans)
{
    kmem_cache_free(pmfs_transaction_cachep, trans);
}

/**********CODE RELATED TO INIT, ALLOC, AND TRANSACT STRUCT*******/

/*Returns the address of per process journal*/
pmfs_journal_t *pvm_set_journal(struct nv_proc_obj *sb){

	if(!sb) {
		printk(KERN_ALERT "pvm_set_journal: procobj NULL\n");
	}
	sb->journal = (pmfs_journal_t *)vmalloc(NVMJOURNALSZ);
	if(!sb->journal) {
		printk(KERN_ALERT "pvm_set_journal: sb->journal alloc failed\n");
		return NULL;
	}
	sb->jsize = NVMJOURNALSZ;	
	return sb->journal;	
}

/*Returns per process journal base data addr*/
uint64_t pvm_get_journal_base(struct nv_proc_obj *sb){

	void *base = NULL;
	if(!sb->journal) {
		printk(KERN_ALERT "pvm_get_journal_base: sb->journal NULL\n");
		return NULL;
	}
	base = sb->journal + sizeof(pmfs_journal_t);
	return (uint64_t)base;
}


void print_journal(struct nv_proc_obj *sb){

	pmfs_journal_t *journal = sb->journal;
	if(!journal) {
		printk(KERN_ALERT "pvm_get_journal_base: sb->journal NULL\n");
		return;
	}
	printk(KERN_ALERT "journal->size %u, "
		"journal->gen_id %u, journal->head %u, ",
		journal->size, journal->gen_id, journal->head);
}

void print_transaction(struct nv_proc_obj *sb, pmfs_transaction_t *trans){

	if(!trans) {
		printk(KERN_ALERT "print_transaction: trans NULL\n");
		return;
	}
    printk(KERN_ALERT "trans id %d, num_les %d, num_used %x\n",
        trans->transaction_id, trans->num_entries,trans->num_used);
}

/*Returns the address of per process journal*/
pmfs_journal_t *pvm_get_journal(struct nv_proc_obj *sb){
	return sb->journal;	
}

int pvm_journal_hard_init(struct nv_proc_obj *sb, uint64_t base,
	uint32_t size)
{
#ifdef LOGWRAPAROUND	
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
#endif

	pmfs_journal_t *journal = pvm_get_journal(sb);
	if(!journal) {
		printk(KERN_ALERT "pvm_journal_hard_init journal NULL\n");
		return -1;
	}
#ifdef LOGWRAPAROUND
	pmfs_memunlock_range(sb, journal, sizeof(*journal));
#endif
	journal->base = cpu_to_le64(base);
	journal->size = cpu_to_le32(size);
	journal->gen_id = cpu_to_le16(1);
	journal->head = journal->tail = 0;
	/* lets do Undo logging for now */
	journal->redo_logging = 0;

#ifdef LOGWRAPAROUND	
	pmfs_memlock_range(sb, journal, sizeof(*journal));
	sbi->journal_base_addr = pmfs_get_block(sb, base);
	pmfs_memunlock_range(sb, sbi->journal_base_addr, size);
	memset_nt(sbi->journal_base_addr, 0, size);
	pmfs_memlock_range(sb, sbi->journal_base_addr, size);
	return pmfs_journal_soft_init(sb);
#endif
	return 0;
}

pmfs_transaction_t *pvm_new_transaction(struct nv_proc_obj *sb,
        int max_log_entries)
{

   pmfs_transaction_t *trans;
    uint32_t head, tail, req_size, avail_size;
    uint64_t base;

    /*get per process journal structure*/
    pmfs_journal_t *journal = pvm_get_journal(sb);
	if(!journal) {
		printk(KERN_ALERT "journal init failed \n");
	}
#ifdef LOGWRAPAROUND
    struct procobj_sb_info *sbi = sb->sb_info;
    /*PVM Comment it for now*/	
    /* If it is an undo log, need one more log-entry for commit record */
    if (!sbi->redo_log)
        max_log_entries++;
#endif
    trans = pvm_alloc_transaction();
    if (!trans)
        return ERR_PTR(-ENOMEM);

    memset(trans, 0, sizeof(*trans));
    trans->num_used = 0;
    trans->num_entries = max_log_entries;
    trans->t_journal = journal;
    req_size = max_log_entries << LESIZE_SHIFT;
#ifdef LOGWRAPAROUND
    mutex_lock(&sb->journal_mutex);
#endif

    tail = le32_to_cpu(journal->tail);
    head = le32_to_cpu(journal->head);
    trans->transaction_id = next_transaction_id++;
again:
    trans->gen_id = le16_to_cpu(journal->gen_id);

    printk(KERN_ALERT "pvm_new_transaction:: "
	"transaction_id %d req_size %u, trans->num_entries %d trans->num_used %u\n",
	trans->transaction_id, req_size, trans->num_entries,trans->num_used);

    avail_size = (tail >= head) ?
        (sb->jsize - (tail - head)) : (head - tail);

    avail_size = avail_size - LOGENTRY_SIZE;

    if (avail_size < req_size) {
        uint32_t freed_size;

#ifdef LOGWRAPAROUND
	/*PVM Comment it for now*/	
        /* run the log cleaner function to free some log entries */
        freed_size = pmfs_free_logentries(max_log_entries);
        if ((avail_size + freed_size) < req_size)
            goto journal_full;
#endif
    }
    base = le64_to_cpu(journal->base) + tail;
    tail = tail + req_size;

#ifdef LOGWRAPAROUND
    /* journal wraparound because of this transaction allocation.
     * start the transaction from the beginning of the journal so
     * that we don't have any wraparound within a transaction */
    pmfs_memunlock_range(sb, journal, sizeof(*journal));

    if (tail >= sbi->jsize) {
        volatile u64 *ptr;
        tail = 0;
        /* write the gen_id and tail atomically. Use of volatile is
         * normally prohibited in kernel code, but it is required here
         * because we want to write atomically against power failures
         * and locking can't provide that. */
        ptr = (volatile u64 *)&journal->tail;
        /* writing 8-bytes atomically setting tail to 0 */
        set_64bit(ptr, (u64)cpu_to_le16(next_gen_id(le16_to_cpu(
                journal->gen_id))) << 32);
        pmfs_memlock_range(sb, journal, sizeof(*journal));
        pmfs_dbg_trans("journal wrapped. tail %x gid %d cur tid %d\n",
            le32_to_cpu(journal->tail),le16_to_cpu(journal->gen_id),
                sbi->next_transaction_id - 1);
        goto again;
    } else {
        journal->tail = cpu_to_le32(tail);
        pmfs_memlock_range(sb, journal, sizeof(*journal));
    }
    mutex_unlock(&sbi->journal_mutex);
#endif
    avail_size = avail_size - req_size;

#ifdef LOGWRAPAROUND
    /* wake up the log cleaner if required */
    if ((sbi->jsize - avail_size) > (sbi->jsize >> 3))
        wakeup_log_cleaner(sbi);
    pmfs_flush_buffer(&journal->tail, sizeof(u64), false);
    pmfs_dbg_trans("new transaction tid %d nle %d avl sz %x sa %llx\n",
        trans->transaction_id, max_log_entries, avail_size, base);
#endif
	trans->start_addr = pvm_get_journal_base(sb);
#ifdef LOGWRAPAROUND
    trans->start_addr = pmfs_get_block(sb, base);
    trans->parent = (pmfs_transaction_t *)current->journal_info;
    current->journal_info = trans;
#endif

    printk(KERN_ALERT "created new transaction tid %d nle %d avl sz %x sa %llx\n",
        trans->transaction_id, max_log_entries, avail_size, base);

    return trans;	

#ifdef LOGWRAPAROUND
journal_full:
    mutex_unlock(&sbi->journal_mutex);
    pmfs_err(sb, "Journal full. base %llx sz %x head:tail %x:%x ncl %x\n",
        le64_to_cpu(journal->base), le32_to_cpu(journal->size),
        le32_to_cpu(journal->head), le32_to_cpu(journal->tail),
        max_log_entries);
    pmfs_free_transaction(trans);
    return ERR_PTR(-EAGAIN);
#endif
}

/* If this is part of a read-modify-write of the block,
 * pvm_memunlock_block() before calling! */
static inline void *pvm_get_dataaddr(struct nv_proc_obj *sb, u64 off)
{
	/*nvpage_start is the pointer to starting address of 
	OS metadata*/
    return off ? ((void *)nvpage_start + off) : NULL;
}


/*Starting address from start of the persistent region
Does not make too much sense now*/
static inline u64
pvm_get_addr_off(struct nv_proc_obj *sb, void *addr)
{
   return addr? (u64)(addr -(void *)nvpage_start) : 0;
}

/* can be called by either during log cleaning or during journal recovery */
static void pvm_flush_transaction(struct nv_proc_obj *sb,
        pmfs_transaction_t *trans)
{
	if(!trans) {
		printk(KERN_ALERT "pvm_flush_transaction NULL trans\n");
		return;
	}

#ifdef LOGWRAPAROUND
    struct pmfs_sb_info *sbi = PMFS_SB(sb);
#endif
    pmfs_logentry_t *le = trans->start_addr;
    int i;
    char *data;

	if(!le) {
		printk(KERN_ALERT "le pointer NULL, error \n");
		return;
	}

    for (i = 0; i < trans->num_used; i++, le++) {
        if (le->size) {
            data = pvm_get_dataaddr(sb,le64_to_cpu(le->addr_offset));
#ifdef LOGWRAPAROUND
            if (sbi->redo_log) {
                pmfs_memunlock_range(sb, data, le->size);
                memcpy(data, le->data, le->size);
                pmfs_memlock_range(sb, data, le->size);
            } else
#endif
			if(data)	
	            pvm_flush_buffer(data, le->size, false);
        }
    }
}

static inline void pmfs_commit_logentry(struct nv_proc_obj *sb,
        pmfs_transaction_t *trans, pmfs_logentry_t *le)
{
#ifdef LOGWRAPAROUND
    struct pmfs_sb_info *sbi = PMFS_SB(sb);
    if (sbi->redo_log) {
        /* Redo Log */
        PERSISTENT_MARK();
        PERSISTENT_BARRIER();
        /* Atomically write the commit type */
        le->type |= LE_COMMIT;
        barrier();
        /* Atomically make the log entry valid */
        le->gen_id = cpu_to_le16(trans->gen_id);
        pmfs_flush_buffer(le, LOGENTRY_SIZE, false);
        PERSISTENT_MARK();
        PERSISTENT_BARRIER();
        /* Update the FS in place */
        pmfs_flush_transaction(sb, trans);
    } else 
#endif
	{
        /* Undo Log */
        /* Update the FS in place: currently already done. so
         * only need to clflush */
        pvm_flush_transaction(sb, trans);
        PERSISTENT_MARK();
        PERSISTENT_BARRIER();
        /* Atomically write the commit type */
        le->type |= LE_COMMIT;
        barrier();
        /* Atomically make the log entry valid */
        le->gen_id = cpu_to_le16(trans->gen_id);
        pvm_flush_buffer(le, LOGENTRY_SIZE, true);
    }
}

int pvm_add_logentry(struct nv_proc_obj *sb,
        pmfs_transaction_t *trans, void *addr, uint16_t size, u8 type)
{
#ifdef LOGWRAPAROUND
    struct pmfs_sb_info *sbi = PMFS_SB(sb);
#endif
    pmfs_logentry_t *le;
    int num_les = 0, i;
#ifdef LOGWRAPAROUND
    uint64_t le_start = size ? pmfs_get_addr_off(sbi, addr) : 0;
#else
    uint64_t le_start = size ? pvm_get_addr_off(sb, addr) : 0;
#endif
    uint8_t le_size;

    if (trans == NULL)
        return -EINVAL;
    le = trans->start_addr + trans->num_used;

    if (size == 0) {
        /* At least one log entry required for commit/abort log entry */
        if ((type & LE_COMMIT) || (type & LE_ABORT)) {
            num_les = 1;
		}
    } else {
        num_les = (size + sizeof(le->data) - 1)/sizeof(le->data);
	}


#ifdef LOGWRAPAROUND
    if ((trans->num_used + num_les) > trans->num_entries) {
        pmfs_err(sb, "Log Entry full. tid %x ne %x tail %x size %x\n",
            trans->transaction_id, trans->num_entries,
            trans->num_used, size);
        dump_transaction(sbi, trans);
        dump_stack();
        return -ENOMEM;
    }
    pmfs_memunlock_range(sb, le, sizeof(*le) * num_les);
#endif

    for (i = 0; i < num_les; i++) {
        le->addr_offset = cpu_to_le64(le_start);
        le->transaction_id = cpu_to_le32(trans->transaction_id);
        le_size = (i == (num_les - 1)) ? size : sizeof(le->data);
        le->size = le_size;
        size -= le_size;
        if (le_size)
            memcpy(le->data, addr, le_size);
        le->type = type;

        if (i == 0 && trans->num_used == 0)
            le->type |= LE_START;
        trans->num_used++;

        /* handle special log entry */
        if (i == (num_les - 1) && (type & LE_COMMIT)) {
			if(trans)
				printk(KERN_ALERT "LE_COMMIT trans->id %u\n", 
					trans->transaction_id);
            pmfs_commit_logentry(sb, trans, le);
#ifdef LOGWRAPAROUND
            pmfs_memlock_range(sb, le, sizeof(*le) * num_les);
#endif
           return 0;
        }
        /* put a compile time barrier so that compiler doesn't reorder
         * the writes to the log entry */
        barrier();

        /* Atomically make the log entry valid */
        le->gen_id = cpu_to_le16(trans->gen_id);
        pvm_flush_buffer(le, LOGENTRY_SIZE, false);
        addr += le_size;
        le_start += le_size;
        le++;
    }
#ifdef LOGWRAPAROUND
    pmfs_memlock_range(sb, le, sizeof(*le) * num_les);
    if (!sbi->redo_log) {
        PERSISTENT_MARK();
        PERSISTENT_BARRIER();
    }
#endif
    return 0;
}

int pvm_commit_transaction(struct nv_proc_obj *sb, pmfs_transaction_t *trans)
{
    if (trans == NULL)
        return 0;
    /* Add the commit log-entry */
    pvm_add_logentry(sb, trans, NULL, 0, LE_COMMIT);
#ifdef LOGWRAPAROUND
    current->journal_info = trans->parent;
#endif
    printk(KERN_ALERT "*********************************\n");
    printk(KERN_ALERT "completing transaction for id %d\n",
        trans->transaction_id);
	print_transaction(sb, trans);
	print_journal(sb);
    printk(KERN_ALERT "*********************************\n");
    pvm_free_transaction(trans);
    return 0;
}


#ifdef NVM_OPTIMIZE_2
void add_to_chunk_cache(struct nv_chunk *chunk, unsigned int vmaid){

	//if(CHUNK_VMACACHESZ > vmacache_cnt) {
		int idx = vmaid % CHUNK_VMACACHESZ;
		chunkid_vma_cache[idx] = (unsigned long)chunk;
		vmacache_cnt++;
	//}
}

struct nv_chunk *get_frm_chunk_cache(unsigned int vmaid){

	int idx = vmaid % CHUNK_VMACACHESZ;
	struct nv_chunk *chunk=NULL;

	if (idx > vmacache_cnt) 
		return NULL;

	chunk = (struct nv_chunk *)chunkid_vma_cache[idx];

	if (chunk && (chunk->vma_id == vmaid))
		return chunk;

	return NULL;
}

void clear_chunk_cache(void) {

	int idx=0;

	for ( idx =0; idx < CHUNK_VMACACHESZ; idx++){
		chunkid_vma_cache[idx]=0;
	}
	vmacache_cnt = 0;
}
#endif

unsigned int jenkin_hash(char *key, unsigned int len)
{
    unsigned int hash, i;
    for(hash = i = 0; i < len; ++i)
    {
        hash += key[i];
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return (unsigned int)hash;
}



#ifdef NVM_OPTIMIZE_2_3
/*Large kmalloc allocation avoids and reduces 
 * TLB miss
 * TODO: the nvpage_start should be per process 
 * instead of one global varialbe*/
void *large_nvstruct_alloc(int size){

	void *ptr = NULL;

	if(!nvpage_start){
		nvpage_start = vmalloc(BATCHSIZE); //kmalloc(BATCHSIZE , GFP_KERNEL);
		if(!nvpage_start) {
			printk(KERN_ALERT "NVM_OPTIMIZE large_nvstruct_alloc "
					"Cannot alloc %u \n",BATCHSIZE);
			goto err_nvstruct_alloc;
		}else {
			printk(KERN_ALERT "large_nvstruct_alloc succeeds \n");
		}	
		nvpage_used = 0;
	}
    ptr  = nvpage_start + nvpage_used;
	nvpage_used = nvpage_used + size;

	if(nvpage_used > BATCHSIZE){
		nvpage_start = NULL;
		printk(KERN_ALERT 
				"large_nvstruct_alloc needs large alloc\n");
	}
	return ptr;
err_nvstruct_alloc:	
	return NULL;
}

void large_nvstruct_free(void){
	if(nvpage_start) {
		kfree(nvpage_start);
		nvpage_start=NULL;
		nvpage_used=0;
	}
}
#endif


/*--------------------------------------------------------------------------------------*/

struct nv_proc_obj * create_proc_obj( unsigned int pid ) {

   struct nv_proc_obj *proc_obj = NULL;
   uint64_t base = 0;	

#ifdef NVM_OPTIMIZE_2_3
    proc_obj =large_nvstruct_alloc(sizeof(struct nv_proc_obj));
    if(!proc_obj)
#endif
   proc_obj = kmalloc(sizeof(struct nv_proc_obj) , GFP_KERNEL);
	if ( !proc_obj ) {
          printk("create_proc_obj:create_proc_obj failed\n");     
          return NULL;
	}

#ifdef NVM_JOURNAL
   pmfs_journal_t *journal;
   pmfs_transaction_t *trans;

    journal = pvm_set_journal(proc_obj);
	base = pvm_get_journal_base(proc_obj);
	pvm_journal_hard_init(proc_obj, base,NVMJOURNALSZ);
    //if (IS_ERR(trans))
      //  return PTR_ERR(trans);
	if((trans =pvm_new_transaction(proc_obj, MAX_LOG_ENTERIES))!= NULL)
		printk(KERN_ALERT "Transaction creation success \n");
	else {
		printk(KERN_ALERT "Transaction creation failed \n");
		goto ret_proc_obj;		
	}
	/*Add proc obj to log entry*/
	if(pvm_add_logentry(proc_obj,trans,
		(void*)proc_obj, sizeof(struct nv_proc_obj), LE_DATA) != 0)
			printk(KERN_ALERT "Transaction log entry failed\n");
#endif
	proc_obj->chunk_initialized = 0;		
	proc_obj->pid = pid;
	//RBchunk tree
	proc_obj->chunk_tree = RB_ROOT;

#ifdef NVM_JOURNAL
	pvm_commit_transaction(proc_obj, trans);
#endif

ret_proc_obj:
	return proc_obj; 
}
EXPORT_SYMBOL(create_proc_obj);

void delete_proc_obj(struct nv_proc_obj *proc_obj) {

	if(&proc_obj->head_proc)
		list_del(&proc_obj->head_proc);

#ifndef NVM_OPTIMIZE_2_3
	if(proc_obj) {
		kfree(proc_obj);
	}
#endif

}
EXPORT_SYMBOL(delete_proc_obj);



int add_proc_obj( struct nv_proc_obj *proc_obj ) {

	if (!proc_obj)
       return 1;
	list_add_tail(&proc_obj->head_proc, &nv_proc_objlist);
	return 0;
}
EXPORT_SYMBOL(add_proc_obj);



/*Func resposible for locating a process object given
 process id.This function assumes that the process object list 
is alreasy initialize*/ 
struct nv_proc_obj *find_proc_obj( unsigned int proc_id ) {

    struct nv_proc_obj *proc_obj = NULL;
    struct nv_proc_obj *tmp_proc_obj = NULL;
    struct list_head *pos = NULL;

    pos = &nv_proc_objlist;
     
	if(!proc_list_init_flag){
		printk("find_proc_obj: proc list not initialized \n");
		goto proc_error;	
 	 }

     list_for_each_entry_safe( proc_obj,tmp_proc_obj, pos, head_proc) {
	 	if ( proc_obj &&  proc_obj->pid == proc_id ) { 
           	 return proc_obj;
		}
     }
#ifdef LOCAL_DEBUG_FLAG
  	 printk("find_proc_obj: no such process \n");
#endif
proc_error:
    return NULL;
}
EXPORT_SYMBOL(find_proc_obj);

/*Method to add new NVRAM process*/
int update_process(unsigned int proc_id, unsigned int chunk_id, size_t chunk_size, 
																	int clear_flg) {
  	struct nv_proc_obj *proc_obj = NULL;	
  	struct rqst_struct rqst;
	struct nv_chunk *chunk = NULL;	

	if (proc_id < 1){
		printk("update_process: Invalid proc id %d \n", proc_id);
		return -1;
	}

	/*if this the first process adding to NVRAM
	then, we do not need to find but just initialize
	the tree*/
	if(!proc_list_init_flag){  
		printk("update_process: initializing new process object list \n");		 
 	    INIT_LIST_HEAD(&nv_proc_objlist);
        proc_list_init_flag = 1;
	}else {
#ifdef LOCAL_DEBUG_FLAG
		printk("update_process: finding object list \n");	
#endif
		proc_obj =	find_proc_obj(proc_id);	
	}

	if(!proc_obj) { 
		//The process has not been already added
	    proc_obj = create_proc_obj(proc_id);	 	
		if(!proc_obj){
			goto error;
		}
		//ok now add it to the process tree
		if (add_proc_obj(proc_obj)){
			printk("update_process:Adding process object failed \n");
			goto error;			
		}
	
#ifdef LOCAL_DEBUG_FLAG	
		printk("update_process: after add_proc_obj \n");
#endif

	}

	/*A metadata for new chunk with 'chunk_size' bytes
	 is added to process chunk list*/
	rqst.pid = proc_id;
	rqst.bytes = chunk_size;
	rqst.id= chunk_id;

	if(clear_flg) {
		//free_chunk(chunk)
		goto create_chunk;
	}else {
		/*check if chunk with this id already exisits?*/
	    chunk = find_chunk( rqst.id, proc_obj );	
	    if(chunk) {	
#ifdef LOCAL_DEBUG_FLAG
			printk("create_add_chunk: chunk %d already exisits\
				 for process %d \n",rqst.id,proc_obj->pid);
#endif
			return 0;
		}
		goto create_chunk;	
	}

create_chunk:
	if(create_add_chunk(proc_obj, &rqst)){
		printk("update_process: adding chunk to process failed \n");
		goto error;
	}    
#ifdef LOCAL_DEBUG_FLAG
	printk("update_process: updated  proc_obj->pid  %d, chunk id : %d  \n", proc_obj->pid, rqst.id);
#endif

	/*Flush cache with fence*/
	pvm_flush_buffer(proc_obj, sizeof(struct nv_proc_obj),ENABLE_FENCE);
	return 0;

error:
#ifdef LOCAL_DEBUG_FLAG
	printk("update_process: adding proc or chunk failed \n");
#endif
	return -1;
}
EXPORT_SYMBOL(update_process);


/*verify if process and chunk exists. Usually used for read only
mode of data */
int find_proc_and_chunk(unsigned int proc_id, unsigned int chunk_id, 
												size_t chunk_size) {

	struct nv_proc_obj *proc_obj = NULL;	
	struct nv_chunk *chunk = NULL;	

	if (proc_id < 1){
		printk("find_proc_and_chunk: Invalid proc id %d \n", proc_id);
		goto find_proc_chunk_err;
	}
	if(!proc_list_init_flag){  
		printk("find_proc_and_chunk: no process yet added \n");		 
		goto find_proc_chunk_err;

	}else {
		proc_obj =	find_proc_obj(proc_id);	
	}
	if(!proc_obj) {
		printk("find_proc_and_chunk: finding proc_obj failed %u\n",proc_id); 
		goto find_proc_chunk_err;
	}
#ifdef LOCAL_DEBUG_FLAG
	printk("find_proc_and_chunk: after acquiring process obj\n");
#endif
	/*check if chunk with this id already exisits?*/
    chunk = find_chunk( chunk_id, proc_obj );	
    if(chunk) {	
		/*check if chunk has any page for reading?*/
		if(chunk->pagecnt) {
#ifdef LOCAL_DEBUG_FLAG
			printk("find_proc_and_chunk: chunk %d exisits\
				 for process %d \n", chunk_id, proc_id);
#endif
			/*return success*/
			return 0;
		}else{
			printk("find_proc_and_chunk: no pages in chunk to read\n");
			goto find_proc_chunk_err;	
		}			
	}
find_proc_chunk_err:
	printk("find_proc_and_chunk: could not find chunk or process\n");
	return -1;
}
EXPORT_SYMBOL(find_proc_and_chunk);



/*creates chunk object.sets object variables to app. value*/
static struct nv_chunk* create_chunk_obj(struct rqst_struct *rqst, struct nv_proc_obj* proc_obj) {

	struct nv_chunk *chunk = NULL;
    
#ifdef LOCAL_DEBUG_FLAG
//	printk "addr %lu  sizeof(chunk) %lu \n", addr, sizeof(chunk));
#endif

#ifdef NVM_OPTIMIZE_2_3
    chunk =large_nvstruct_alloc(sizeof(struct nv_chunk));
    if(!chunk)
#endif
	 chunk = kmalloc(sizeof(struct nv_chunk) , GFP_KERNEL);
     if ( !chunk ) {
          printk("create_proc_obj:create_proc_obj failed\n");     
          return NULL;
     }

	chunk->vma_id =  rqst->id;
	chunk->length = rqst->bytes;
    chunk->proc_id = rqst->pid;

	//Initializing the page list header
	//for this structure
	//dont need to check for list initialization
	//elsewhere
    INIT_LIST_HEAD( &chunk->nv_used_page_list);
    /*Initialize the rbtree node*/	
	chunk->page_tree = RB_ROOT;
    //Set the list flag
    chunk->used_page_list_flg = 1;
	chunk->pagecnt = 0;
	//optimization field 
	chunk->max_pg_offset = 0;
#ifdef LOCAL_DEBUG_FLAG
	printk( "Setting offset chunk->vma_id %d \n",chunk->vma_id);
#endif

    /*Flush cache with fence*/
    pvm_flush_buffer(chunk, sizeof(struct nv_chunk),ENABLE_FENCE);
	return chunk;
}

void delete_nvchunk(struct nv_chunk *chunk) {
	if(chunk) {
		kfree(chunk);
	}
}
EXPORT_SYMBOL(delete_nvchunk);

/*Function to return the process object to which chunk belongs
@ chunk: process to which the chunk belongs
@ return: process object 
 */
struct nv_proc_obj* get_process_obj(struct nv_chunk *chunk) {

	if(!chunk) {
		printk( "get_process_obj: chunk null \n");
		return NULL;
	}
	return chunk->proc_obj;
}

/*Function to find the chunk.
@ process_id: process identifier
@ var:  variable which we are looking for
 */
struct nv_chunk* find_chunk( unsigned int vma_id, struct nv_proc_obj *proc_obj) {

	struct rb_root *root = NULL ;
    struct rb_node *node = NULL; 
    unsigned int chunkid;
#ifdef NVM_OPTIMIZE_2
	struct nv_chunk *chunk=NULL;
#endif

#ifdef LOCAL_DEBUG_FLAG 
	printk( "find_chunk vma_id:%u \n",vma_id);
#endif

#ifdef LOCAL_DEBUG_FLAG
	if(proc_obj)
		printk("total chunks proc_obj->num_chunks:%u \n",proc_obj->num_chunks);
#endif

	if(!proc_obj) {
		printk( "could not identify project id \n");
		goto find_chunk_error;
	}

	if (!proc_obj->chunk_initialized){
#ifdef LOCAL_DEBUG_FLAG
		printk("find_chunk: proc %d chunk list not initialized \n", proc_obj->pid);
#endif
		goto find_chunk_error;
	}

    root = &proc_obj->chunk_tree;
	if(!root){
		printk("find_chunk: root is null \n");
		goto find_chunk_error;
	}
	node = root->rb_node;
	if(!node) {
		printk("find_chunk: node is null \n");
		goto find_chunk_error;
	}

#ifdef NVM_OPTIMIZE_2
	chunk = (struct nv_chunk *)get_frm_chunk_cache(vma_id);

	if(chunk && (chunk->vma_id == vma_id))
		return chunk;
#endif

    while (node) {
        struct nv_chunk *this = rb_entry(node, struct nv_chunk, rb_chunknode);
        if(!this) {
            goto find_chunk_error;
        }
        chunkid = this->vma_id;
        if( vma_id == chunkid) {
            return this;
        }
        if ( vma_id < chunkid) {
            node = node->rb_left;

        }else if ( vma_id > chunkid){
            node = node->rb_right;
        }
    }
    goto find_chunk_error;

find_chunk_error:
#ifdef LOCAL_DEBUG_FLAG
    printk( "find_chunk: could not find chunk\n");
#endif
	return NULL;
}

/*add the chunk to process object*/
static int add_chunk(struct nv_chunk *chunk, struct nv_proc_obj *proc_obj) {

	struct rb_node **new = NULL, *parent = NULL;
	struct rb_root *root = NULL ;

    if (!chunk)
       return -1;
	if(!proc_obj)
		return -1;
    if (!proc_obj->chunk_initialized){
        INIT_LIST_HEAD(&proc_obj->chunk_list);
        proc_obj->chunk_initialized = 1;
    }
    root = &proc_obj->chunk_tree;
	if(!root){
		printk("add_chunk: new is null \n");
		return -1;
	}
	new = &(root->rb_node);
	if(!new) {
		printk("add_chunk: new is null \n");
		return -1;
	}
	/* Figure out where to put new node */
	while (*new) {
        struct nv_chunk *this = rb_entry(*new, struct nv_chunk, rb_chunknode);
        parent = *new;
        if(!this) {
            return -1;
        }
        if (chunk->vma_id < this->vma_id ) {
            new = &((*new)->rb_left);
        }else if (chunk->vma_id > this->vma_id){
            new = &((*new)->rb_right);
        }else{
            return -1;
        }
    }
    /* Add new node and rebalance tree. */
    rb_link_node(&chunk->rb_chunknode, parent, new);
    rb_insert_color(&chunk->rb_chunknode, root);

#ifdef NVM_OPTIMIZE_2
	add_to_chunk_cache(chunk, chunk->vma_id);
#endif

    /*set the process obj to which chunk belongs*/
    chunk->proc_obj = proc_obj;
    /*Flush cache with fence*/
    pvm_flush_buffer(chunk, sizeof(struct nv_chunk),ENABLE_FENCE);
    return 0;
}


/*Every NValloc call creates a chunk and each chunk
 is added to process object list
@clear: indicates, if exisiting chunks needs to be cleared?*/
static int create_add_chunk(struct nv_proc_obj *proc_obj, struct rqst_struct *rqst) {

	struct nv_chunk *chunk = NULL;
#ifdef NVM_JOURNAL
	pmfs_transaction_t *trans;
#endif
	if(!proc_obj){
		printk("create_add_chunk: proc_obj invalid \n");		
		goto add_to_process_error; 
	}
	chunk = create_chunk_obj( rqst, proc_obj);
    /*add chunk to process object*/
    if (!chunk){
		printk("create_add_chunk: chunk creating failed \n");		
		goto add_to_process_error;
	}
#ifdef NVM_JOURNAL
	trans =pvm_new_transaction(proc_obj, MAX_LOG_ENTERIES);
  	if (IS_ERR(trans))
  		return PTR_ERR(trans);

    /*Add proc obj to log entry*/
    if(pvm_add_logentry(proc_obj,trans,
        (void*)proc_obj, sizeof(struct nv_proc_obj), LE_DATA) != 0)
            printk(KERN_ALERT "proc_obj log entry failure\n");

	/*Add chunk obj to log entry*/
    if(pvm_add_logentry(proc_obj,trans,
        (void*)chunk, sizeof(struct nv_chunk), LE_DATA) != 0)
            printk(KERN_ALERT "chunk log entry failure \n");
#endif

	add_chunk(chunk, proc_obj);
    proc_obj->num_chunks++;

#ifdef NVM_JOURNAL
	pvm_commit_transaction(proc_obj, trans);
#endif

    return 0;
add_to_process_error:
	return -1;	
}


/*Function to copy chunks */
/*static int chunk_copy(struct nv_chunk *dest, struct nv_chunk *src){ 

	if(!dest ) {
		printk("chunk_copy:dest is null \n");
		goto null_err;
	}

	if(!src ) {
		printk("chunk_copy:src is null \n");
		goto null_err;
	}

	//TODO: this is not a great way to copy structures
	dest->vma_id = src->vma_id;
	dest->length = src->length;
	dest->proc_id = src->proc_id;
    //print_chunk(dest);
	//FIXME:operations should be structure
	// BUT how to map them

	return 0;

	null_err:
	return -1;	
}*/

/*add the chunk to process object*/
struct nv_chunk* find_chunk_using_vma( struct vm_area_struct *vma){

    unsigned int proc_id;
    unsigned int chunk_id;
    struct nv_proc_obj *proc_obj = NULL;
    struct nv_chunk *chunk = NULL;

    /*This code heavily depends on VMA
    and it should contain all information*/
    proc_id = vma->proc_id;
    chunk_id = vma->vma_id;

#ifdef LOCAL_DEBUG_FLAG
   printk("find_chunk_using_vma: proc_id %d & chunk_id: %d \n", (int)proc_id, (int)chunk_id);
#endif

    if(!proc_id || !chunk_id)
        goto add_pages_error;

    proc_obj= find_proc_obj(proc_id);
    if(!proc_obj){
        printk("find_chunk_using_vma: finding proc_obj failed \n");
        goto add_pages_error;
    }

    chunk = find_chunk(chunk_id, proc_obj);
    if(!chunk) {
        printk("find_chunk_using_vma: chunk is null\n");
         goto add_pages_error;
    }

#ifdef LOCAL_DEBUG_FLAG
   printk("find_chunk_using_vma: proc_id %d & chunk_id: %d \n", (int)proc_id, (int)chunk_id);
#endif
    return chunk;

add_pages_error:
#ifdef LOCAL_DEBUG_FLAG
    printk("find_chunk_using_vma: failed \n");
#endif
    return NULL;
}




unsigned int find_offset(struct vm_area_struct *vma, unsigned long addr) {

	unsigned int offset = 0;		
	unsigned int pg_off = 0;

	if ((offset = (addr - vma->vm_start)) < 0){
    	printk("find_offset: offset %u is incorrect \n",offset);
    }
    /*find the page number*/
    pg_off = offset >> PAGE_SHIFT;
#ifdef LOCAL_DEBUG_FLAG
    printk("find_offset: offset %u, page offset %u \n", offset, pg_off);
#endif
	return pg_off;
}

#ifdef NVM_OPTIMIZE_3
unsigned int cache_insrt_procid;
unsigned int cache_insrt_vmaid;
struct rb_node *cache_insrt_node;
unsigned int cache_insrt_pgoff;
#endif


#ifdef NVM_OPTIMIZE_3
struct nvpage *insrt_page_cache_node(unsigned int nvpgoff,
							unsigned int vmaid,
							unsigned int procid){

	struct rb_node *temp = NULL;
	struct nvpage *nxtpg= NULL;

    if(procid && vmaid && (cache_insrt_procid == procid) &&
        (cache_insrt_vmaid == vmaid) &&
        cache_insrt_node && 
		cache_insrt_pgoff+1 == nvpgoff){

		temp = rb_next(cache_insrt_node);

		if(temp)	
			nxtpg = rb_entry(temp, struct nvpage, rbnode);

		if(nxtpg && nxtpg->page && nxtpg->page->nvpgoff == nvpgoff) {
			cache_insrt_node = &nxtpg->rbnode;
			cache_insrt_pgoff = nxtpg->page->nvpgoff;
			return nxtpg;				
		}
    }
	return NULL;	
}
#endif


/*add pages to rbtree node */
int insert_page_rbtree(struct rb_root *root,struct page *page, 
						struct nv_chunk *chunk){

	struct rb_node **new = &(root->rb_node), *parent = NULL;
	struct nvpage *nvpage = NULL;

#ifdef NVM_JOURNAL
    pmfs_transaction_t *trans;
#endif

#ifdef NVM_OPTIMIZE_2_3
	nvpage =large_nvstruct_alloc(sizeof(struct nvpage));
	if(!nvpage)
#endif
	nvpage = kmalloc(sizeof(struct nvpage) , GFP_KERNEL);
	if(!nvpage) {
		printk("insert_page_rbtree: nvpage struct alloc failed \n");
		return -1;
	}

#ifdef NVM_JOURNAL
    trans =pvm_new_transaction(chunk->proc_obj, MAX_LOG_ENTERIES);
    if (IS_ERR(trans))
        return PTR_ERR(trans);

     /*Add chunk obj to log entry*/
    if(pvm_add_logentry(chunk->proc_obj,trans,
        (void*)chunk, sizeof(struct nv_chunk), LE_DATA) != 0) {
            printk(KERN_ALERT "chunk log entry failure \n");
		goto insert_fail_nvpg;
	}

    /*Add chunk obj to log entry*/
    if(pvm_add_logentry(chunk->proc_obj,trans,
        (void*)nvpage, sizeof(struct nvpage), LE_DATA) != 0) {
            printk(KERN_ALERT "nvpage log entry failure \n");
		goto insert_fail_nvpg;
	}
#endif

	nvpage->page = page;
	if(!(nvpage->page)){
		printk("insert_page_rbtree: page to insert NULL \n");
		goto insert_fail_nvpg;
	}

	/* Figure out where to put new node */
	while (*new) {

		struct nvpage *this = rb_entry(*new, struct nvpage, rbnode);
		parent = *new;

		if(!this || !this->page) {
			printk("insert_page_rbtree: chunk rbtree not initialized \n");
			goto insert_fail_nvpg;
		}
		if (nvpage->page->nvpgoff < this->page->nvpgoff) {
			new = &((*new)->rb_left);
			pvm_flush_buffer(new, CACHELINE_SIZE, ENABLE_FENCE);
		}else if (nvpage->page->nvpgoff > this->page->nvpgoff){
			new = &((*new)->rb_right);
			pvm_flush_buffer(new, CACHELINE_SIZE, ENABLE_FENCE);
		}else{
			goto insert_success_nvpg;
		}
	}
	/* Add new node and rebalance tree. */
	rb_link_node(&nvpage->rbnode, parent, new);
	rb_insert_color(&nvpage->rbnode, root);

#ifdef LOCAL_DEBUG_FLAG
	printk("insert_page_rbtree: success\n");
#endif

	pvm_flush_buffer(nvpage, sizeof(struct nvpage), ENABLE_FENCE);

insert_success_nvpg:
#ifdef NVM_JOURNAL
    pvm_commit_transaction(chunk->proc_obj, trans);
#endif	
	return 0;

insert_fail_nvpg:
#ifdef NVM_JOURNAL
    pvm_commit_transaction(chunk->proc_obj, trans);
#endif
	return -1;
}


#if 0
/* This version cotains all the optimizations*/
/*add pages to rbtree node */
#ifdef NVM_OPTIMIZE_3
int insert_page_rbtree(struct rb_root *root, struct page *page, 
						unsigned int vmaid, unsigned int procid){
#else
int insert_page_rbtree(struct rb_root *root, struct page *page){
#endif

	struct rb_node **new = &(root->rb_node), *parent = NULL;
	struct nvpage *nvpage = NULL;


#ifdef NVM_JOURNAL
    pmfs_transaction_t *trans;
#endif

#ifdef NVM_OPTIMIZE_2_3
	nvpage =large_nvstruct_alloc(sizeof(struct nvpage));
	if(!nvpage)
#endif
	nvpage = kmalloc(sizeof(struct nvpage) , GFP_KERNEL);
	if(!nvpage) {
		printk("insert_page_rbtree: nvpage struct alloc failed \n");
		return -1;
	}

#ifdef NVM_JOURNAL
    trans =pvm_new_transaction(proc_obj, MAX_LOG_ENTERIES);
    if (IS_ERR(trans))
        return PTR_ERR(trans);

     /*Add chunk obj to log entry*/
    if(pvm_add_logentry(proc_obj,trans,
        (void*)chunk, sizeof(struct nv_chunk), LE_DATA) != 0)
            printk(KERN_ALERT "chunk log entry failure \n");


    /*Add chunk obj to log entry*/
    if(pvm_add_logentry(proc_obj,trans,
        (void*)nvpage, sizeof(struct nvpage), LE_DATA) != 0)
            printk(KERN_ALERT "nvpage log entry failure \n");
#endif
	nvpage->page = page;
	if(!(nvpage->page)){
		printk("insert_page_rbtree: page to insert NULL \n");
		return -1;
	}
#ifdef NVM_OPTIMIZE_3
	struct nvpage *tmpnvpage = NULL;
	tmpnvpage = insrt_page_cache_node(nvpage->page->nvpgoff, vmaid, procid);
	if(tmpnvpage && tmpnvpage->page &&  
			(tmpnvpage->page->nvpgoff ==  page->nvpgoff)) {
		*new = &tmpnvpage->rbnode;
	}
#endif
	/* Figure out where to put new node */
	while (*new) {

		struct nvpage *this = rb_entry(*new, struct nvpage, rbnode);
		parent = *new;

		if(!this || !this->page) {
#ifdef LOCAL_DEBUG_FLAG
			printk("insert_page_rbtree: chunk rbtree not initialized \n");
#endif
			return -1;
		}
		if (nvpage->page->nvpgoff < this->page->nvpgoff) {
			new = &((*new)->rb_left);
			pvm_flush_buffer(new, CACHELINE_SIZE, ENABLE_FENCE);
		}else if (nvpage->page->nvpgoff > this->page->nvpgoff){
			new = &((*new)->rb_right);
			pvm_flush_buffer(new, CACHELINE_SIZE, ENABLE_FENCE);
		}else{
#ifdef LOCAL_DEBUG_FLAG
			printk("insert_page_rbtree: already exists, do nothing \n");
#endif
#ifdef NVM_OPTIMIZE_3
			cache_insrt_procid = procid;
			cache_insrt_vmaid = vmaid;
			cache_insrt_node = *new;
			cache_insrt_pgoff = nvpage->page->nvpgoff;
#endif
			return 0;
		}
	}
	/* Add new node and rebalance tree. */
	rb_link_node(&nvpage->rbnode, parent, new);
	rb_insert_color(&nvpage->rbnode, root);
#ifdef LOCAL_DEBUG_FLAG
	printk("insert_page_rbtree: success\n");
#endif
	pvm_flush_buffer(nvpage, sizeof(struct nvpage), ENABLE_FENCE);
	return 0;
}
#endif


#ifdef NVM_OPTIMIZE_1
unsigned int cache_srch_procid;
unsigned int cache_srch_vmaid;
struct rb_node *cache_srch_node;
unsigned int cache_srch_pgoff;

struct page *search_cache_page(unsigned int nvpgoff,
							unsigned int vmaid,
							unsigned int procid){

	struct rb_node *temp = NULL;
	struct nvpage *nxtpg= NULL;

    if(procid && vmaid && (cache_srch_procid == procid) &&
        (cache_srch_vmaid == vmaid) &&
        cache_srch_node && 
		cache_srch_pgoff+1 == nvpgoff){

		temp = rb_next(cache_srch_node);

		if(temp)	
			nxtpg = rb_entry(temp, struct nvpage, rbnode);

		if(nxtpg && nxtpg->page && nxtpg->page->nvpgoff == nvpgoff) {
			cache_srch_node = &nxtpg->rbnode;
			cache_srch_pgoff = nxtpg->page->nvpgoff;
			return nxtpg->page;				
		}
    }
	return NULL;	
	
	/*printk("cache_srch_procid %u procid %u "
			"cache_srch_vmaid %u : vmaid %u "
			"cache_srch_pgoff %u : nvpgoff %u "
			"cache_srch_node %lu \n",
			cache_srch_procid, procid, cache_srch_vmaid,
			vmaid, cache_srch_pgoff, nvpgoff,cache_srch_node);*/
}
#endif

#ifdef NVM_OPTIMIZE_1
struct page *search_page_rbtree(struct rb_root *root, 
								unsigned int nvpgoff, 
								unsigned int vmaid, 
								unsigned int procid) {
#else
struct page *search_page_rbtree(struct rb_root *root, unsigned int nvpgoff){
#endif

	struct rb_node *node = root->rb_node;
	unsigned int offset;
	struct page *page = NULL;

#ifdef NVM_OPTIMIZE_1
	page = search_cache_page(nvpgoff,  vmaid, procid);

	if(page)
		return page;
#endif

	while (node) {

		struct nvpage *this = rb_entry(node, struct nvpage, rbnode);

		if(!this || this->page == NULL) {
#ifdef LOCAL_DEBUG_FLAG
			printk("search_page_rbtree: chunk rbtree not initialized \n");
#endif
			return NULL;
		}

		offset = this->page->nvpgoff;

		if( nvpgoff == offset) {
			page = this->page;

#ifdef NVM_OPTIMIZE_1
		
		cache_srch_procid = procid;
		cache_srch_vmaid = vmaid;

		if(node){
			cache_srch_node = node;
		}
		else{
			cache_srch_node = NULL;
		}

		if(page){
		 	cache_srch_pgoff = page->nvpgoff;
		 }	
#endif
			goto ret_search_page;
		}

		if ( nvpgoff < offset) 
            node = node->rb_left;
        else if (nvpgoff > offset)
            node = node->rb_right;
	}
#ifdef NVM_OPTIMIZE_1
	cache_srch_node = NULL;
#endif

	return NULL;

ret_search_page:
	return page;


}


/*add the chunk to process object
Called should ensure that chunk page list is initialized*/

int add_pages_to_chunk(struct vm_area_struct *vma, struct page *page, unsigned long addr){

	unsigned int proc_id;
    unsigned int chunk_id;
	struct nv_chunk *chunk = NULL;
	unsigned int nvpgoff = 0;
	//struct nvpage nvpage;

	if(!vma) {
		printk("add_pages_to_chunk:null vma err \n");
		goto add_pages_error;
	}

	if( !page ) {
		printk("add_pages_to_chunk:null page err \n");
		goto add_pages_error;
	}

	if (vma->persist_flags != PERSIST_VMA_FLAG ){
		printk("add_pages_to_chunk:not a NVRAM vma \n");
		goto add_pages_error;
	}

	if(!test_bit(PG_nvram, &page->flags)){
		printk("add_pages_to_chunk:not a NVRAM page \n");
		goto add_pages_error;		
	}

	/*This code heavily depends on VMA
	and it should contain all information*/
	proc_id = vma->proc_id;
    chunk_id = vma->vma_id;

	if(!proc_id || !chunk_id){
		printk("add_pages_to_chunk: invalid chunk or procid \n");
		goto add_pages_error;
	}

	chunk = find_chunk_using_vma(vma);
	if(!chunk){	
#ifdef LOCAL_DEBUG_FLAG
		printk("add_pages_to_chunk: finding chunk failed\n");
#endif
		goto add_pages_error;	
	}

	nvpgoff =  find_offset(vma, addr);
	page->nvpgoff = nvpgoff;

#ifdef NVM_OPTIMIZE_3
	if(insert_page_rbtree( &chunk->page_tree, page, chunk_id, proc_id)) {
#else
	if(insert_page_rbtree( &chunk->page_tree, page, chunk)) {
#endif

#ifdef LOCAL_DEBUG_FLAG
		printk("inserting page with offset %d failed \n",page->nvpgoff);
#endif
		goto add_pages_error;
	}
	/*Next time if some other search with larger offset
	come, use this field to say no*/
	if( chunk->max_pg_offset < page->nvpgoff);
		chunk->max_pg_offset = page->nvpgoff;
	/*successful addition of page to chunk list
	increment counter*/
	chunk->pagecnt++;

#ifdef NV_DIRTYPG_TRCK
	chunk->dirtpgs++;
#endif

#ifdef LOCAL_DEBUG_FLAG
	printk("add_pages_to_chunk: proc_id %d"
		   "chunk_id: %d, pagecnt:%d"
			"page->nvpgoff %u\n", 
			(int)proc_id, (int)chunk_id, 
			chunk->pagecnt,
			page->nvpgoff);
#endif
    return 0;
add_pages_error:
	return -1;
}
EXPORT_SYMBOL(add_pages_to_chunk);


int check_page_exists(struct nv_chunk *chunk, long offset, long *pg_off){

	if(!chunk){
		printk("check_page_exists: chunk null \n");
		goto page_chck_error;
	}

	//FIXME: There should be a better way to find incorrect address
	if( offset < 0) {
		printk("check_page_exists: incorrect fault addr %ld \n", offset);
		 //goto page_chck_error;
		 offset = 0;
	}

	 /*find the page number*/
    *pg_off = offset >> PAGE_SHIFT;

#ifdef LOCAL_DEBUG_FLAG
    printk("check_page_exists: fault_addr %lu, page offset %ld \n", offset, *pg_off);
#endif

	if( chunk->pagecnt > *pg_off) {
		printk("check_page_exists: page already exisits in this offset \n");
		return EXISTS;
	}

	return DOES_NOT_EXIST;

page_chck_error:
	return UNEXP_ERR;
}



struct page* find_page(unsigned int proc_id, unsigned int chunk_id, unsigned int pg_off)
{
	struct nv_chunk *chunk = NULL;
	struct page *page = NULL;
	//unsigned int hash=0;
	struct nv_proc_obj *proc_obj;

	if(!proc_id || !chunk_id){
#ifdef LOCAL_DEBUG_FLAG
		 printk("find_page: unexpected proc_id %u,\
					chunk_id %u \n",proc_id, chunk_id);
#endif
		goto err_nv_faultpg;
	}

    proc_obj=find_proc_obj(proc_id);
    if(!proc_obj) {
        printk(KERN_ALERT "process with pid: %u"
                          "does not exist \n",proc_id);
         goto err_nv_faultpg;
    }

	chunk = find_chunk(chunk_id, proc_obj);
	if(!chunk){	
#ifdef LOCAL_DEBUG_FLAG
		printk("find_page: finding chunk failed\n");
#endif
		goto err_nv_faultpg;	
	}

#ifdef NVM_OPTIMIZE
	 if( chunk->max_pg_offset < pg_off ) {
		goto err_nv_faultpg;
	}
#endif

	/*get the page from corresponsing offset*/
	if ( (page = get_page_frm_chunk(chunk, pg_off)) == NULL){
#ifdef LOCAL_DEBUG_FLAG
        printk("find_page: get_page_frm_chunk failed \n");
#endif
        goto err_nv_faultpg;
    }
#ifdef LOCAL_DEBUG_FLAG
	printk("find_page: proc_id %d, chunk_id: %d \n",
			(int)proc_id, (int)chunk_id);
#endif
     return page;

err_nv_faultpg:
	return NULL;
}


struct page* get_nv_faultpg(struct vm_area_struct *vma, 
							unsigned long fault_addr,
							 int *err){

	unsigned int proc_id = 0;
    unsigned int chunk_id = 0;
	struct nv_chunk *chunk = NULL;
	long pg_off = -1;
	struct page *page = NULL;
    long offset = 0;

	/*This code heavily depends on VMA
	and it should contain all information*/
	proc_id = vma->proc_id;
    chunk_id = vma->vma_id;
	if(!proc_id || !chunk_id){
		*err = UNEXP_ERR;
#ifdef LOCAL_DEBUG_FLAG
		 printk("get_nv_faultpg: unexpected proc_id %u,\
					chunk_id %u \n",proc_id, chunk_id);
#endif
		goto err_nv_faultpg;
	}
	chunk = find_chunk_using_vma(vma);
	if(!chunk){	
#ifdef LOCAL_DEBUG_FLAG
		printk("KERN_ALERT get_nv_faultpg: finding chunk failed\n");
#endif
		*err = UNEXP_ERR;
		goto err_nv_faultpg;	
	}
   if ((offset = (fault_addr - vma->vm_start)) < 0){
#ifdef LOCAL_DEBUG_FLAG
        printk("KERN_ALERT nv_read: offset  %lu is incorrect \n",offset);
#endif
		 *err = UNEXP_ERR;
		goto err_nv_faultpg;
    }

    pg_off = find_offset(vma,fault_addr);

	if(pg_off < 0) {
#ifdef LOCAL_DEBUG_FLAG
		printk("KERN_ALERT get_nv_faultpg: offset error \n");
#endif
		*err = UNEXP_ERR;
		goto err_nv_faultpg;
	}
	/*get the page from corresponsing offset*/
	if ( (page = get_page_frm_chunk(chunk, pg_off)) == NULL){

#ifdef LOCAL_DEBUG_FLAG
        printk("KERN_ALERT get_nv_faultpg: get_page_frm_chunk failed \n");
#endif
		*err = UNEXP_ERR;
        goto err_nv_faultpg;
    }
#ifdef LOCAL_DEBUG_FLAG
     printk("get_nv_faultpg: found page, returning page \n");
#endif
	 *err = 0;

#ifdef NV_STATS_DEBUG
    g_nvpg_read_cnt++;
    print_global_nvstats();
#endif

#ifdef LOCAL_DEBUG_FLAG
	printk("get_nv_faultpg: proc_id %d "
		   "chunk_id: %d, fault_addr %lu "
		   " vma->vm_start %lu "	
			"page->nvpgoff %u\n", 
			(int)proc_id, (int)chunk_id, 
			fault_addr,
			vma->vm_start,
			page->nvpgoff);
#endif
	 //find_page(proc_id, chunk_id, 0);
     return page;

err_nv_faultpg:
#ifdef LOCAL_DEBUG_FLAG_1
	if(vma) 
	printk("FAILED get_nv_faultpg: proc_id %d "
		   "chunk_id: %d, fault_addr %lu "
		   " vma->vm_start %lu, pg_off %lu \n", 
			(int)proc_id, (int)chunk_id, 
			fault_addr,
			vma->vm_start, 
			pg_off);
#endif
	return NULL;

}
EXPORT_SYMBOL(get_nv_faultpg);

//Method to get a page from the chunk list
struct page* get_page_frm_chunk(struct nv_chunk *chunk, long pg_off){

    struct page *page = NULL;

	if(!chunk){
		printk("get_page_frm_chunk: chunk null\n");
		goto page_frm_chunk_err;		
	}

	/*Check if page list for this chunk is already initialized
	If the list is not initialized, then no point in further
	looking for page.so return NULL*/
	//FIXME: use unlikely
    if(!chunk->used_page_list_flg){
#ifdef LOCAL_DEBUG_FLAG
		printk("get_page_frm_chunk: chunk list not initialized \n");
#endif
		goto page_frm_chunk_err; 
	}
	
	if(!chunk->pagecnt){
#ifdef LOCAL_DEBUG_FLAG
		printk("get_page_frm_chunk: no pages in chunk yet \n");
#endif
		goto page_frm_chunk_err;	
	}

	 if( chunk->max_pg_offset < pg_off ) {
		 //No point iterating the tree.
		//we will definitely not find the page
#ifdef LOCAL_DEBUG_FLAG_1
		printk("get_page_frm_chunk: req pageoff %u "
				"greater that max %u \n", pg_off, 
				chunk->max_pg_offset);
#endif
		goto page_frm_chunk_err;
	}

#ifdef NVM_OPTIMIZE_1
	page = search_page_rbtree(&chunk->page_tree, pg_off, 
								chunk->vma_id, chunk->proc_id);
#else
	page = search_page_rbtree(&chunk->page_tree, pg_off);
#endif

    if(!page){   
  		goto page_frm_chunk_err;
	}
	else {
#ifdef LOCAL_DEBUG_FLAG
		printk("get_page_frm_chunk: page found \n");
#endif
		return page;	
	}

page_frm_chunk_err:
#ifdef LOCAL_DEBUG_FLAG
	printk("get_page_frm_chunk: page with off %u not "
			"found \n", pg_off);
#endif
	return NULL;
}


/*This method uses the VMA and fault address to identify the persistent page
Uses the VMA start address and fault address to identify the chunk and
corresponding page*/
struct page* nv_read(struct vm_area_struct *vma, unsigned long fault_addr) {

    unsigned long offset = 0;
    unsigned int proc_id = 0 ;
    unsigned int chunk_id;
	struct nv_chunk *chunk = NULL;
	unsigned int pg_off = 0;
	struct page *page = NULL;
#ifdef NV_STATS_DEBUG	
	struct nv_proc_obj *proc_obj;
#endif

    proc_id = vma->proc_id;
    chunk_id = vma->vma_id;

#ifdef LOCAL_DEBUG_FLAG
	 printk("nv_read: proc_id %d & chunk_id: %d \n", (int)proc_id, (int)chunk_id);
#endif

    if(!proc_id || !chunk_id)
        goto error;

	chunk = find_chunk_using_vma(vma);
    if(!chunk){
        printk("nv_read: finding chunk failed\n");
        goto error;
    }

	/*using the fault address calculate the page address
	CAUTION: Here, we assume that all pages in our process-chunk
	list are aligned sequentially (i.e.) addr 0...4096...8192
     */
	 if ((offset = (fault_addr - vma->vm_start)) < 0){
		printk("nv_read: offset  %lu is incorrect \n",offset);
	}

	/*find the page number*/
	pg_off = offset >> PAGE_SHIFT;
	/*get the page from chunk and return it to caller.*/
	if ( (page = get_page_frm_chunk(chunk, pg_off)) == NULL){
		printk("nv_read: finding page failed \n");
		goto error;
	}
#ifdef LOCAL_DEBUG_FLAG
	 printk("nv_read: found page, returning page \n");
#endif
#ifdef NV_STATS_DEBUG
	printk("nv_read: in NV_STATS_DEBUG condition \n");
	//proc_obj = get_process_obj(chunk);	
	//proc_obj->read_pgcnt++;
	g_nvpg_read_cnt++;
	print_global_nvstats();
	//print_proc_nvstats(proc_obj);
#endif

     return page;
error:
    return NULL;

}
EXPORT_SYMBOL(nv_read);

/*SYSCALL_DEFINE3(nv_commit, unsigned long addr, unsigned long len,
                                    struct nvmap_arg_struct  *nvarg)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma, *prev;
	struct rb_node **rb_link, *rb_parent;

	vma = find_vma_prepare(mm, addr, &prev, &rb_link, &rb_parent);
	if(!vma) {
    	 printk("addr does not exist");
	     goto error;	
	 }

	flush_cache_range(mm, addr, len);

	printk("nv_commit: after flush \n");

	return 0;

error:
	printk("incorrect vma address \n");
	return -1;
}*/


int intialize_flag = 0;

//System call to allocate a fixed pool
//returns 0 on successful allocation
//SYSCALL_DEFINE1(nvpoolcreate, unsigned long, num_pool_pages)
asmlinkage long sys_nvpoolcreate( unsigned long num_pool_pages)
{
    int status = -1;

    if(!num_pool_pages)
        return -1;

#ifdef NVM_OPTIMIZE_1
	clear_chunk_cache();
#endif

    //RESERVE NV mem pool
	printk("nvpoolcreate: allocate request for %lu \n",num_pool_pages);
    status = alloc_fresh_nv_pages( NULL, num_pool_pages);
    if (!status){
      	intialize_flag = 1;
		g_nv_init_pgs += num_pool_pages;
        goto poolcreate_success;
	} else {
		goto error;
	}

poolcreate_success:
#ifdef BACKING_STORE
	nvm_load_from_file(BACKINGFILE);
#endif

#ifdef NVM_JOURNAL
	init_transaction_cache();
#endif

	printk("nvpoolcreate: allocation satisfied \n");
	return status;

error:  
	printk("nvpoolcreate: failed\n"); 
    return status;
}

/*system call to intialize persistent pages
*of a process
*@param: inode: the inode - unique identifier of process
*@return, 0 - Successful intialization, -1 failed */
SYSCALL_DEFINE1(initpmem, unsigned long, inode)
{
	int ret = -1;

	if(inode <=0) {
		printk("initpmem: invalid inode \n");
		goto error;
	}

	printk("initpmem: entering\n");
    ret = init_proc_nvpages(inode);
    if(!ret)
		printk("initpmem: success\n");
	else {
		printk("initpmem: failure\n");
		goto error;
	}
	return ret;
error:	
	return -1;
}


#ifdef NV_STATS_DEBUG
void print_proc_nvstats(struct nv_proc_obj *proc_obj) {

	if(!proc_obj) {
		printk(KERN_EMERG "Invalid process object\n");
		return;
	}
	printk("=========PROCESS STATS================\n");
    printk("STATS Proc ID %d \n", proc_obj->pid);
	printk("#. process maps:%d \n",proc_obj->num_chunks);
	//printk("#. of pages:%u\n", proc_obj->num_pages); 
	//printk("#. reused pages:%u\n", proc_obj->read_pgcnt); 
	return;
}

void print_chunk_stats(struct nv_chunk *chunk) {

	if(!chunk) {
		printk(KERN_EMERG "Invalid chunk\n");
		return;
	}
	printk("chunk: vma_id %u\n", chunk->vma_id);
    printk("chunk: length %ld\n",  chunk->length);
	printk("=============================");
	return;
}
void print_global_nvstats(void) {

	printk("=========GLOBAL STATS========\n");
	printk("g_used_nvpg %u \n",g_used_nvpg);
	printk("g_num_persist_pg %u \n",g_num_persist_pg);
	printk("g_nvpg_read_cnt %u \n", g_nvpg_read_cnt);
}

#endif

#ifdef NV_DIRTYPG_TRCK
unsigned int copy_dirtpg(struct rb_root *root, 
						unsigned int  __user *destbuf,
						unsigned int dirtpgs) {

    struct rb_node *node;
	int ret = 0;
    unsigned int cnt = 0, pg_copied=0;
	unsigned int *pagelist, *ktemp;

	if(unlikely(!destbuf)){
		printk(KERN_ALERT "locate_dirtypage_rbtree:" 
				"Bad dest addr \n");
		goto iterate_error;	
	}

    if(unlikely(!root)) {
		printk(KERN_ALERT "invalid chunk page tree \n");
        goto iterate_error;
    }

	for (node = rb_first(root); node; node = rb_next(node)){
        struct nvpage *this = rb_entry(node, struct nvpage, rbnode);
        if(!this || this->page == NULL) 
            break;

        if( this->page->nvdirty){
            cnt++;
		    if (copy_to_user(destbuf, &this->page->nvpgoff,sizeof(unsigned int))) {
    			ret = -EFAULT;
			    printk(KERN_ALERT "copy_to_user failed \n");
			    goto out_free;  
		    }else{
				destbuf = destbuf + sizeof(unsigned int);
			}

			/*reset the pages to not dirty*/
			this->page->nvdirty = 0;

	     }else{
			printk("page not dirty \n");
		 }
	}
out_free:
    printk("copy_dirtpg: copied %u pages\n", cnt);
    return cnt;

iterate_error:
    printk("copy_dirtpg: failed \n");
    return 0;


			/*copy required flags*/
			/*ktempg = (char *)kmalloc(PAGE_SIZE, GFP_KERNEL);
			if(!ktempg){
				printk(KERN_ALERT "error allocing kernel page \n");
				goto iterate_error;
			}
			temp = (char *)ktempg;
			for ( i = 0; i < 4096; i++)
		        printk(KERN_ALERT "temp: %c\n", (char)temp[i]);

			memcpy(ktempg, this->page,PAGE_SIZE);
		    if(copy_from_user(ktempg, this->page,PAGE_SIZE)){
                 ret = -EFAULT;
				 printk(KERN_ALERT "copy_from_user failed \n");
                 goto out_free;
            }
			for ( i = 0; i < 4096; i++)
		        printk(KERN_ALERT "ktempg: %c\n", (char)ktempg[i]);*/

			/*copy required flags*/
			//ktempg->nvpgoff = this->page->nvpgoff;

			/*now copy to user space */
			/*if (copy_to_user(destbuf, ktempg, PAGE_SIZE)) {
				ret = -EFAULT;
				printk(KERN_ALERT "copy_to_user failed \n");
				goto out_free;	
			}*/
			 /*for ( i = 0; i < 4096; i++)
                printk("destbuf: %c\n", (char)destbuf[i] );*/

			//destpg = (struct page *)destbuf;
			//destpg->nvpgoff = ktempg->nvpgoff;
			/*pg_copied++;
			printk("copy_dirtpg: pg_copied %u\n",pg_copied);
			destbuf = destbuf + PAGE_SIZE;
			kfree(ktempg);
        }*/
}


asmlinkage long sys_copydirtpages(
			struct nvmap_arg_struct *nvarg, 
			unsigned int  __user *dest)
{
	unsigned int chunkid = nvarg->chunkid;
	unsigned int procid = nvarg->procid;
    struct nv_proc_obj *proc_obj;
	struct nv_chunk *chunk = NULL;	

    proc_obj=find_proc_obj(procid);	
	if(!proc_obj) {
         printk(KERN_ALERT "process with pid: %u"
				"does not exist \n",procid);
         goto error;
    }
	chunk = find_chunk( chunkid, proc_obj );
    if(!chunk) {
    	printk(KERN_ALERT "copydirtpages: chunk %u for procid"
			    "%u does not exist\n", chunkid, procid);
        goto error;
    }
	if(&chunk->page_tree == NULL){
		printk(KERN_ALERT "copydirtpages:chunk rbtree root" 
				"null error \n");
		goto error;	
	}
	return copy_dirtpg(&chunk->page_tree, dest, chunk->dirtpgs);	
error:
    return 0;
}

#endif //NV_DIRTYPG_TRCK




/*This system call is for debugging any new kernel functionalites*/

//SYSCALL_DEFINE1(NValloc, unsigned long, numpgs)
asmlinkage long sys_NValloc( unsigned long numpgs)
{

#ifndef NOXEN_HETERO
#ifdef HETEROMEM
   int rc;
   struct page **pages;
   printk(KERN_ALERT "sys_NValloc: numpgs %lu \n",numpgs);
   
   if(numpgs == 0){
		alloc_xenheteromemed_pages(numpgs, pages, 1, 1);
		return 0;
   }	
   printk(KERN_ALERT "sys_NValloc: before alloc_xenheteromemed_pages \n");	

   rc = alloc_xenheteromemed_pages(numpgs, pages, 1, 0);
   if (rc != 0) {
           pr_warn("%s Could not alloc %d pfns rc:%d\n", __func__,
                   numpgs, rc);
           kfree(pages);
           return -ENOMEM;
	}else {
		printk("Successfuly allocated pages \n");
	}
   	return 0;
#endif
#endif

    printk(KERN_ALERT "sys_NValloc: invoking iterate_chunk \n");
	sys_clear_all_persist(numpgs);
	return 0;	
}


asmlinkage long sys_clear_all_persist( unsigned long proc_id){

	struct nv_proc_obj *proc_obj = NULL;
	struct nv_proc_obj *tmp_proc_obj=NULL;
	struct list_head *pos = NULL;

	pos = &nv_proc_objlist;

    if(!proc_list_init_flag || !pos){
        printk("sys_clear_all_persist: proc list not initialized \n");
		goto endclear;
     }

	if(proc_id == 0){
		printk(KERN_ALERT "sys_clear_all_persist:all persistent state \n");
	    list_for_each_entry_safe( proc_obj,tmp_proc_obj, pos, head_proc) 
		if ( proc_obj) {
			iterate_chunk(proc_obj);
#ifdef LOCAL_DEBUG_FLAG_1
			printk("sys_clear_all_persist: clearing pages for proc %u\n", 
					proc_obj->pid);
#endif
     	}
#ifdef NVM_OPTIMIZE_2_3
		large_nvstruct_free();
#endif
	}else {
		proc_obj= find_proc_obj(proc_id);
	    if(!proc_obj){
    	    printk("sys_clear_all_persist: finding proc_obj failed \n");
        	goto endclear;
    	}

		if (!proc_obj->chunk_initialized){
			printk("sys_clear_all_persist: proc %d chunk list not initialized \n", proc_obj->pid);
			goto endclear;
		}
		iterate_chunk(proc_obj);
	}
	
endclear:
	return 0;
}

struct page *delete_page_rbtree(struct rb_root *root){

    struct rb_node *node = root->rb_node;

    for (node = rb_first(root); node != NULL;
             node = rb_next(node)) {

        struct nvpage *this = rb_entry(node, struct nvpage, rbnode);
        if(!this || this->page == NULL) {
            printk("delete_page_rbtree: chunk rbtree not initialized \n");
            return NULL;
        }
        rb_erase(node, root);
        add_to_free_nvlist(this->page); 
    }	
    return NULL;
}

int delete_pages_in_chunk(struct vm_area_struct *vma){

    unsigned int proc_id;
    unsigned int chunk_id;
    struct nv_chunk *chunk = NULL;

    if(!vma) {
        printk("delete_pages_error:null vma err \n");
        goto delete_pages_error;
    }

    if (vma->persist_flags != PERSIST_VMA_FLAG ){
        printk("delete_pages_error:not a NVRAM vma \n");
        goto delete_pages_error;
    }

    /*This code heavily depends on VMA
    and it should contain all information*/
    proc_id = vma->proc_id;
    chunk_id = vma->vma_id;

    if(!proc_id || !chunk_id){
        printk("delete_pages_error: invalid chunk or procid \n");
        goto delete_pages_error;
    }
    chunk = find_chunk_using_vma(vma);

    if(!chunk){
        printk("delete_pages_error: finding chunk failed\n");
        goto delete_pages_error;
    }
	delete_page_rbtree(&chunk->page_tree);
	return 0;
delete_pages_error:
	return -1;
}
EXPORT_SYMBOL(delete_pages_in_chunk);


struct page *clear_page_data(struct rb_root *root){

    struct rb_node *node = root->rb_node;

    for (node = rb_first(root); node != NULL;
             node = rb_next(node)) {
 
        struct nvpage *this = rb_entry(node, struct nvpage, rbnode);
        if(!this || this->page == NULL) {
            printk("clear_page_data: chunk rbtree not initialized \n");
            return NULL;
        }
	    //printk("clear_page_data: clearing page %u\n", this->page->nvpgoff);

		//spin_lock(&nv_proclist_lock);
        rb_erase(node, root);
		//clear_page(this->page);	
        add_to_free_nvlist(this->page); 
		//spin_unlock(&nv_proclist_lock);
     }	
    return NULL;
}


/*Function to find the chunk.
@ process_id: process identifier
@ var:  variable which we are looking for
 */
struct nv_chunk* iterate_chunk(struct nv_proc_obj *proc_obj) {

    struct rb_root *root = NULL;
    struct rb_node *node = NULL; 
    //unsigned int chunkid;
   
    root = &proc_obj->chunk_tree;
	if(!root){
		printk("iterate_chunk: root is null \n");
		goto finish_iter;
	}

    for (node = rb_first(root); node != NULL;
		node = rb_next(node)) {

        struct nv_chunk *this = rb_entry(node, struct nv_chunk, rb_chunknode);
      	if(!this) {
            goto finish_iter;
        }
		//printk("iterate_chunk: proc id %u deleting chunk %u and " 
		//		"pages \n",proc_obj->pid, this->vma_id);

		clear_page_data(&this->page_tree);
		rb_erase(node, root);
#ifndef NVM_OPTIMIZE_2_3
		delete_nvchunk(this);
#endif
    }

#ifdef NVM_OPTIMIZE_2
	large_nvstruct_free();
	clear_chunk_cache();
#endif 

	delete_proc_obj(proc_obj);

finish_iter:
#ifdef LOCAL_DEBUG_FLAG_1
    printk( "find_chunk: could not find chunk\n");
#endif
	return NULL;
}

/****************************BACKING STORE*********************/

#ifdef BACKING_STORE

static ssize_t nvm_write_backing_store(struct file *flp, char *src,
        ssize_t bytes, loff_t *woff)
{
    mm_segment_t old_fs;
    ssize_t len = 0;

    if (bytes > 0) {
        old_fs = get_fs();
        set_fs(get_ds());
        len = vfs_write(flp, src, bytes, woff);
        set_fs(old_fs);
        if (len <= 0)
            printk("nvm_write_backing_store: "
					"Could not write file or corrupted\n");
    }
    return len;
}

static ssize_t nvm_read_backing_store(struct file *flp, char *dest,
    ssize_t bytes, loff_t *roff)
{
    mm_segment_t old_fs;
    ssize_t len = 0;

    if (bytes > 0) {
        old_fs = get_fs();
        set_fs(get_ds());
        len = vfs_read(flp, dest, bytes, roff);
        set_fs(old_fs);
        if (len <= 0)
            printk("nvm_write_backing_store: "
					"Could not read file or corrupted\n");
    }
    return len;
}

void nvm_load_from_file(char *nvm_backing_file)
{
    struct file *flp;
    mm_segment_t oldfs;

    if (nvm_backing_file && strlen(nvm_backing_file)) {
        oldfs = get_fs();
        set_fs(get_ds());
        flp = filp_open(nvm_backing_file, O_RDONLY | O_LARGEFILE,
            S_IRWXU);
        set_fs(oldfs);
        if (IS_ERR(flp)) {
            printk("nvm_load_from_file: failed open %s\n",
                   nvm_backing_file);
        } else {
            printk("nvm_load_from_file: open success %s\n",
                   nvm_backing_file);
#if 0
            pmfs_loadfs(flp, sb);
            oldfs = get_fs();
            set_fs(get_ds());
            filp_close(flp, current->files);
            set_fs(oldfs);
#endif
        }
    }
}

#if 0
static void *nv_frag_start(struct seq_file *m, loff_t *pos)
{
    return NULL;
}

static const struct seq_operations nvmap_info_op = {
    .start  = nv_frag_start, 
    .next   = nv_frag_start,
    .stop   = nv_frag_start,
    .show   = nv_frag_start,
};

static int nvmap_open(struct inode *inode, struct file *file)
{
    return seq_open(file, &nvmap_info_op);
}c


static const struct file_operations proc_nvmap_operations = {
    .open       = nvmap_open,
    .read       = seq_read,
    .llseek     = seq_lseek,
    .release    = seq_release,
};


static int __init setup_nvmaps(void)
{
#ifdef CONFIG_PROC_FS
    proc_create("nvmaps", S_IRUGO, NULL, &proc_nvmap_operations);
#endif
    return 0;
}
module_init(setup_nvmaps)
#endif

#endif

/****************************BACKING STORE*********************/

/****************************BACKING STORE*********************/

/*****************************UNUSED FUNCTIONS*********************/
#if 0
/* This is needed, because copy_page and memcpy are not usable for copying
* task structs.
 */
static inline void do_copy_page(long *dst, long *src)
{
        int n;

        for (n = PAGE_SIZE / sizeof(long); n; n--)
                *dst++ = *src++;
}

static int safe_copy_page(void *dst, struct page *s_page)
{
      if (kernel_page_present(s_page)) { 
             do_copy_page(dst, page_address(s_page));
			return 0;	
	 }else {
			return UNEXP_ERR;	
	 }	
 }

/*Method to set the chunk process information*/
/*static int set_process_chunk(struct nv_chunk *chunk, unsigned int proc_id) {

	if(!chunk){
		printk( "chunk is null \n");
		goto error;
	}
	//set the info     
	chunk->proc_id = proc_id;
	return 0;

	error:
	return -1;	

}*/

/*Method to get the chunk process information*/
/*static int get_process_id(struct nv_chunk *chunk) {

	if(!chunk){
		printk( "chunk is null \n");
                return -1;
	}
	return chunk->proc_id;

 }*/
#endif
