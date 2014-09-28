#include <linux/kernel_stat.h>
#include <linux/mm.h>
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


//#define HETEROMEM

//#define LOCAL_DEBUG_FLAG
//#define NV_STATS_DEBUG

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


/*--------------------------------------------------------------------------------------*/

//static function list

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

#ifdef NV_STATS_DEBUG
	void print_global_nvstats(void);
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



/*--------------------------------------------------------------------------------------*/

struct nv_proc_obj * create_proc_obj( unsigned int pid ) {

   struct nv_proc_obj *proc_obj = NULL;

   proc_obj = kmalloc(sizeof(struct nv_proc_obj) , GFP_KERNEL);
	if ( !proc_obj ) {
          printk("create_proc_obj:create_proc_obj failed\n");     
          return NULL;
	}
	proc_obj->chunk_initialized = 0;		
    proc_obj->pid = pid;
	//RBchunk tree
	proc_obj->chunk_tree = RB_ROOT;
	return proc_obj; 

}
EXPORT_SYMBOL(create_proc_obj);


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

    //spin_lock(&nv_proclist_lock);

    pos = &nv_proc_objlist;
     
	if(!proc_list_init_flag){
		printk("find_proc_obj: proc list not initialized \n");
		goto proc_error;	
 	 }

     list_for_each_entry_safe( proc_obj,tmp_proc_obj, pos, head_proc) {
	 	if ( proc_obj &&  proc_obj->pid == proc_id ) { 
			 //spin_unlock(&nv_proclist_lock);
           	 return proc_obj;
		}
     }

#ifdef LOCAL_DEBUG_FLAG
  	 printk("find_proc_obj: no such process \n");
#endif
proc_error:
	//spin_unlock(&nv_proclist_lock);
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
#ifdef LOCAL_DEBUG_FLAG
		printk("update_process: after creating  new process object list \n");
#endif

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
#ifdef LOCAL_DEBUG_FLAG
    printk("update_process: before create_add_chunk \n");
#endif
	if(create_add_chunk(proc_obj, &rqst)){
		printk("update_process: adding chunk to process failed \n");
		goto error;
	}    
#ifdef LOCAL_DEBUG_FLAG
	printk("update_process: after create_add_chunk proc_obj->pid  %d, chunk id : %d  \n", proc_obj->pid, rqst.id);
#endif
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
		printk("find_proc_and_chunk: finding proc_obj failed \n"); 
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
	 chunk = kmalloc(sizeof(struct nv_chunk) , GFP_KERNEL);
     if ( !chunk ) {
          printk("create_proc_obj:create_proc_obj failed\n");     
          return NULL;
     }

	if(chunk == NULL) {
		printk("chunk creation failed\n");
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
	//current offset of process
	//*curr_offset = *curr_offset + length;
	return chunk;
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

    //set the process obj to which chunk belongs 
    chunk->proc_obj = proc_obj;
    return 0;
}


/*Every NValloc call creates a chunk and each chunk
 is added to process object list
@clear: indicates, if exisiting chunks needs to be cleared?*/
static int create_add_chunk(struct nv_proc_obj *proc_obj, struct rqst_struct *rqst) {

	struct nv_chunk *chunk = NULL;

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
	add_chunk(chunk, proc_obj);
    proc_obj->num_chunks++;
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


/*add pages to rbtree node */
int insert_page_rbtree(struct rb_root *root, struct page *page){

	struct rb_node **new = &(root->rb_node), *parent = NULL;
	struct nvpage *nvpage = NULL;

	nvpage = kmalloc(sizeof(struct nvpage) , GFP_KERNEL);
	if(!nvpage) {
		printk("insert_page_rbtree: nvpage struct alloc failed \n");
		return -1;
	}
	nvpage->page = page;
	if(!(nvpage->page)){
		printk("insert_page_rbtree: page to insert NULL \n");
		return -1;
	}
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
		}else if (nvpage->page->nvpgoff > this->page->nvpgoff){
			new = &((*new)->rb_right);
		}else{
#ifdef LOCAL_DEBUG_FLAG
			printk("insert_page_rbtree: already exists, do nothing \n");
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
	return 0;
}


struct page *search_page_rbtree(struct rb_root *root, unsigned int nvpgoff){

	struct rb_node *node = root->rb_node;
	unsigned int offset;

	while (node) {
		struct nvpage *this = rb_entry(node, struct nvpage, rbnode);
		if(!this || this->page == NULL) {
#ifdef LOCAL_DEBUG_FLAG
			printk("search_page_rbtree: chunk rbtree not initialized \n");
#endif
			return NULL;
		}
		offset = this->page->nvpgoff;
		if( nvpgoff == offset) 
			return this->page;
		if ( nvpgoff < offset) 
            node = node->rb_left;
        else if (nvpgoff > offset)
            node = node->rb_right;
	}
	return NULL;
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

	if(insert_page_rbtree( &chunk->page_tree, page)) {
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

struct page* find_page(unsigned int proc_id, unsigned int chunk_id, unsigned int pg_off)
{
	struct nv_chunk *chunk = NULL;
	struct page *page = NULL;
	char *buffer = NULL;
	unsigned int hash=0;
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

	/*get the page from corresponsing offset*/
	if ( (page = get_page_frm_chunk(chunk, pg_off)) == NULL){

#ifdef LOCAL_DEBUG_FLAG
        printk("find_page: get_page_frm_chunk failed \n");
#endif
        goto err_nv_faultpg;
    }

	//buffer = (char *)kmalloc(PAGE_SIZE, GFP_KERNEL);	
	//if(!safe_copy_page(buffer, page))
	//hash = jenkin_hash(buffer, PAGE_SIZE);	

#ifdef LOCAL_DEBUG_FLAG
	printk("find_page: proc_id %d, chunk_id: %d, hash:%u \n",
			(int)proc_id, (int)chunk_id, hash);
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
		printk("get_nv_faultpg: finding chunk failed\n");
#endif
		*err = UNEXP_ERR;
		goto err_nv_faultpg;	
	}
   if ((offset = (fault_addr - vma->vm_start)) < 0){
        printk("nv_read: offset  %lu is incorrect \n",offset);
		 *err = UNEXP_ERR;
		goto err_nv_faultpg;
    }
    pg_off = find_offset(vma,fault_addr);
	if(pg_off < 0) {
		printk("get_nv_faultpg: offset error \n");
		*err = UNEXP_ERR;
		goto err_nv_faultpg;
	}
	/*get the page from corresponsing offset*/
	if ( (page = get_page_frm_chunk(chunk, pg_off)) == NULL){
#ifdef LOCAL_DEBUG_FLAG
        printk("get_nv_faultpg: get_page_frm_chunk failed \n");
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
	
	find_page(proc_id, chunk_id, 0);

     return page;
err_nv_faultpg:
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
#ifdef LOCAL_DEBUG_FLAG
		printk("get_page_frm_chunk: req pageoff greater that max\n");
#endif
		goto page_frm_chunk_err;
	}


	page = search_page_rbtree(&chunk->page_tree, pg_off);
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
	printk("get_page_frm_chunk: page not found \n");
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
    //RESERVE NV mem pool
	printk("nvpoolcreate: allocate request for %lu \n",num_pool_pages);
    status = alloc_fresh_nv_pages( NULL, num_pool_pages);
    if (!status){
      	intialize_flag = 1;
        printk("nvpoolcreate: allocation satisfied \n");
		g_nv_init_pgs += num_pool_pages;
        return status;
	} else {
		goto error;
	}
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
	/*if(xen_do_alloc_heteropg(10, 0)){
		printk("heteromem alloc failed \n");			
	}else{
		printk("heteromem alloc succeeded\n");			
	}*/

   int rc;
   struct page **pages;

   printk(KERN_ALERT "sys_NValloc: numpgs %lu \n",numpgs);

   /*pages = kmalloc(numpgs*sizeof(struct page), GFP_KERNEL);
   if (pages == NULL) {
		printk("sys_NValloc: Allocation failed \n"); 
        return -ENOMEM;
	}*/

   printk(KERN_ALERT "sys_NValloc: kalloc \n");	


	//send_hotpage_skiplist();
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
}




