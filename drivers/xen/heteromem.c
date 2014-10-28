/******************************************************************************
 * Xen heteromem driver - enables returning/claiming memory to/from Xen.
 *
 * Copyright (c) 2003, B Dragovic
 * Copyright (c) 2003-2004, M Williamson, K Fraser
 * Copyright (c) 2005 Dan M. Smith, IBM Corporation
 * Copyright (c) 2010 Daniel Kiper
 *
 * Memory hotplug support was written by Daniel Kiper. Work on
 * it was sponsored by Google under Google Summer of Code 2010
 * program. Jeremy Fitzhardinge from Citrix was the mentor for
 * this project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/*SAME CODE AS BALLONING. All worker scheduling is turned off
Also all code related to hotplug has been removed */


#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/bootmem.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/gfp.h>
#include <linux/notifier.h>
#include <linux/memory.h>
#include <linux/memory_hotplug.h>

#include <asm/page.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include <asm/tlb.h>

#include <asm/xen/hypervisor.h>
#include <asm/xen/hypercall.h>
#include <xen/xen.h>
#include <xen/interface/xen.h>
#include <xen/interface/memory.h>
#include <xen/balloon.h>
#include <xen/heteromem.h>
#include <xen/features.h>
#include <xen/page.h>
#include <linux/mm.h>
#include <linux/migrate.h>

/*Debug flag*/
//#define HETERODEBUG

/* HeteroMem node to allocate from. */
#define XENMEMF_node(x)     (((x) + 1) << 8)
#define XENMEMF_get_node(x) ((((x) >> 8) - 1) & 0xffu)
#define XENMEMF_hetero_mem_request  (1<<18)
#define XENMEMF_hetero_stop_hotpage_scan (1<<19)

#define MAX_HOT_MFN 16384
#define MAX_MIGRATE 4096

/*
 * heteromem_process() state:
 *
 * BP_DONE: done or nothing to do,
 * BP_EAGAIN: error, go to sleep,
 * BP_ECANCELED: error, heteromem operation canceled.
 */

enum bp_state {
	BP_DONE,
	BP_EAGAIN,
	BP_ECANCELED
};


static DEFINE_MUTEX(heteromem_mutex);

struct heteromem_stats heteromem_stats;
EXPORT_SYMBOL_GPL(heteromem_stats);

/* We increase/decrease in batches which fit in a page */
///static xen_pfn_t *hetero_frame_list;
static xen_pfn_t hetero_frame_list[MAX_HOT_MFN];
//static xen_pfn_t hetero_frame_listshadow[PAGE_SIZE];


#ifdef CONFIG_HIGHMEM
#define inc_totalhigh_pages() (totalhigh_pages++)
#define dec_totalhigh_pages() (totalhigh_pages--)
#else
#define inc_totalhigh_pages() do {} while (0)
#define dec_totalhigh_pages() do {} while (0)
#endif

/* List of heteromemed pages, threaded through the mem_map array. */
static LIST_HEAD(heteromemed_pages);
/*pages for which host, pfn-to-mfn table has been updated*/
static LIST_HEAD(hetero_ready_lst_pgs);
/*pages which needs to be deleted*/
static LIST_HEAD(hetero_used_lst_pgs);

/*hetero ready page list count*/
static unsigned int ready_lst_pgcnt;
/*hetero used page list count*/
static unsigned int used_lst_pgcnt;
static unsigned int dbg_resv_hetropg_cnt;
static unsigned int hotpagecnt;
static unsigned int mfnmatchcnt;


/* Main work function, always executed in process context. */
static void heteromem_process(struct work_struct *work);
static DECLARE_DELAYED_WORK(heteromem_worker, heteromem_process);

void debug_heteroused_page(void);

/* When heteromeming out (allocating memory to return to Xen) we don't really
   want the kernel to try too hard since that can trigger the oom killer. */
#define GFP_BALLOON \
	(GFP_HIGHUSER | __GFP_NOWARN | __GFP_NORETRY | __GFP_NOMEMALLOC)

static void scrub_page(struct page *page)
{
#ifdef CONFIG_XEN_SCRUB_PAGES
	clear_highpage(page);
#endif
}

/* heteromem_append: add the given page to the heteromem. */
static void __heteromem_append(struct page *page)
{
	/* Lowmem is re-populated first, so highmem pages go at list tail. */
	if (PageHighMem(page)) {
		list_add_tail(&page->lru, &heteromemed_pages);
		heteromem_stats.heteromem_high++;
	} else {
		list_add(&page->lru, &heteromemed_pages);
		heteromem_stats.heteromem_low++;
	}
}

static void heteromem_append(struct page *page)
{
	__heteromem_append(page);
	if (PageHighMem(page))
		dec_totalhigh_pages();
	totalram_pages--;
}

/* heteromem_retrieve: rescue a page from the heteromem, if it is not empty. */
static struct page *heteromem_retrieve(bool prefer_highmem)
{
	struct page *page;

	if (list_empty(&heteromemed_pages))
		return NULL;

	if (prefer_highmem)
		page = list_entry(heteromemed_pages.prev, struct page, lru);
	else
		page = list_entry(heteromemed_pages.next, struct page, lru);

	list_del(&page->lru);

	if (PageHighMem(page)) {
		heteromem_stats.heteromem_high--;
		inc_totalhigh_pages();
	} else
		heteromem_stats.heteromem_low--;

	totalram_pages++;

	return page;
}

static struct page *heteromem_first_page(void)
{
	if (list_empty(&heteromemed_pages))
		return NULL;
	return list_entry(heteromemed_pages.next, struct page, lru);
}

static struct page *heteromem_next_page(struct page *page)
{
	struct list_head *next = page->lru.next;
	if (next == &heteromemed_pages)
		return NULL;
	return list_entry(next, struct page, lru);
}

static enum bp_state update_schedule(enum bp_state state)
{
	if (state == BP_DONE) {
		heteromem_stats.schedule_delay = 1;
		heteromem_stats.retry_count = 1;
		return BP_DONE;
	}

	++heteromem_stats.retry_count;

	if (heteromem_stats.max_retry_count != RETRY_UNLIMITED &&
			heteromem_stats.retry_count > heteromem_stats.max_retry_count) {
		heteromem_stats.schedule_delay = 1;
		heteromem_stats.retry_count = 1;
		return BP_ECANCELED;
	}

	heteromem_stats.schedule_delay <<= 1;

	if (heteromem_stats.schedule_delay > heteromem_stats.max_schedule_delay)
		heteromem_stats.schedule_delay = heteromem_stats.max_schedule_delay;

	return BP_EAGAIN;
}

static long current_credit(void)
{
	unsigned long target = heteromem_stats.target_pages;

	target = min(target,
		     heteromem_stats.current_pages +
		     heteromem_stats.heteromem_low +
		     heteromem_stats.heteromem_high);

	return target - heteromem_stats.current_pages;
}

static bool heteromem_is_inflated(void)
{
	if (heteromem_stats.heteromem_low || heteromem_stats.heteromem_high)
		return true;
	else
		return false;
}

static enum bp_state reserve_additional_memory(long credit)
{
	heteromem_stats.target_pages = heteromem_stats.current_pages;
	return BP_DONE;
}

static enum bp_state increase_reservation(unsigned long nr_pages, struct page **pagearr)
{
	int rc;
	unsigned long  pfn, i;
	struct page   *page;
	struct xen_memory_reservation reservation= {
		.address_bits = 0,
		.extent_order = 0,
		.domid        = DOMID_SELF,
		.mem_flags	  = 0
	};

	if(!hetero_frame_list) {
		printk(KERN_ALERT "hetero_frame_list alloc failed \n");
		return BP_EAGAIN; 
	}

	//if (nr_pages > ARRAY_SIZE(hetero_frame_list))
		//nr_pages = ARRAY_SIZE(hetero_frame_list);
	nr_pages = MAX_HOT_MFN;

	page = heteromem_first_page();
	for (i = 0; i < nr_pages; i++) {
		if (!page) {
			nr_pages = i;
			break;
		}
		hetero_frame_list[i] = page_to_pfn(page);
		page = heteromem_next_page(page);
	}


		/*Setting the heteromem request flag used in hypervisor*/
		reservation.mem_flags = reservation.mem_flags|XENMEMF_hetero_mem_request;
		/*HETEROFIX: HARDCODE*/
    	reservation.mem_flags = reservation.mem_flags|XENMEMF_node(1);
	
		set_xen_guest_handle(reservation.extent_start, hetero_frame_list);
		reservation.nr_extents = nr_pages;

		rc = HYPERVISOR_memory_op(XENMEM_hetero_populate_physmap, &reservation);
		if (rc <= 0){
			printk(KERN_DEBUG "XENMEM_populate_physmap failed %d\n", rc);
			return BP_EAGAIN;
		}
#ifdef HETERODEBUG
		printk(KERN_DEBUG "XENMEM_hetero_populate_physmap succeeded for "
					  "%u pages \n", nr_pages);
		printk(KERN_DEBUG "reserv done for rc %d pages, nr_pages %lu\n",rc, nr_pages);
#endif

	for (i = 0; i < rc; i++) {
		page = heteromem_retrieve(false);
		BUG_ON(page == NULL);

		pfn = page_to_pfn(page);
		BUG_ON(!xen_feature(XENFEAT_auto_translated_physmap) &&
		       phys_to_machine_mapping_valid(pfn));

		set_phys_to_machine(pfn, hetero_frame_list[i]);
#ifdef HETERODEBUG
	printk(KERN_ALERT "increase_reservation: pfn[%d]:%u "
			"ready_lst_pgcnt %u hetero reserv pages %u\n", 
			i, pfn, ready_lst_pgcnt, dbg_resv_hetropg_cnt);
#endif


#ifdef CONFIG_XEN_HAVE_PVMMU
		/* Link back into the page tables if not highmem. */
		if (xen_pv_domain() && !PageHighMem(page)) {
			int ret;
			ret = HYPERVISOR_update_va_mapping(
				(unsigned long)__va(pfn << PAGE_SHIFT),
				mfn_pte(hetero_frame_list[i], PAGE_KERNEL),
				0);
			BUG_ON(ret);
		}
#endif
		/* We will not relinquish the page back to the allocator. 
		Because we mange the pages*/
		//ClearPageReserved(page);
		//init_page_count(page);
		//__free_page(page);
		//SetPageReserved(page);	

		list_add(&page->lru, &hetero_ready_lst_pgs);
		ready_lst_pgcnt++;
		dbg_resv_hetropg_cnt++;

		/*Fill it only if it is not NULL */
		if(pagearr)
			pagearr[i] = page;
		
	}
	heteromem_stats.current_pages += rc;
#ifdef HETERODEBUG
	printk(KERN_DEBUG "Increase reservation %d "
			" ready_lst_pgcnt %u \n", rc, ready_lst_pgcnt);
#endif
	return BP_DONE;
}


struct list_head *glbnext;
int first_time =0;


static struct page *heteroused_first_page(void)
{
	if (list_empty(&hetero_used_lst_pgs))
		return NULL;
	return list_entry(hetero_used_lst_pgs.next, struct page, lru);
}

static struct page *heteroused_next_page(struct page *page)
{

	struct page *debug_page;
	struct list_head *debug_next;

	struct list_head *next = page->lru.next;
	if (next == &hetero_used_lst_pgs)
		return NULL;

	/*debug_page = list_entry(next, struct page, lru);	
	if(debug_page) {
		 printk(" pfn: %u \n",
         page_to_pfn(debug_page));
		 debug_next = debug_page->lru.next;
		 if(debug_next){
			debug_page= list_entry(debug_next, struct page, lru);
			 if(debug_page)
			 printk(" next pfn: %u \n",
	         page_to_pfn(debug_page));
		 } 	
	}*/

	return list_entry(next, struct page, lru);
}


struct page* get_from_usedpage_list() { 

	struct page *page = NULL;

	/*if(!first_time) {
		glbnext = &hetero_used_lst_pgs;
		first_time = 1; 
	 }*/	
	 if (list_empty(&hetero_used_lst_pgs)){
    	 printk(KERN_DEBUG "hetero_getused_page: list is empty\n");
	     return NULL;
	 }
	 page = list_entry(hetero_used_lst_pgs.prev, struct page, lru);
	 if(!page) {
    	printk(KERN_DEBUG "hetero_getused_page: list is empty \n");
	    return NULL;
	  }
	  //glbnext = glbnext->next;
	  return page; 	
}


int send_hotpage_skiplist()
{
		enum bp_state state = BP_DONE;
		unsigned long  pfn, i;
		struct page *page = NULL;
		/*pages for which host, pfn-to-mfn table has been updated*/
		struct list_head *hetero_skiplst;
		unsigned int nr_pages;
		int ret = 0;	

		printk("calling send_hotpage_skiplist\n");

		struct xen_memory_reservation reservation = {
				.address_bits = 0,
				.extent_order = 0,
				.domid        = DOMID_SELF
		};

		hetero_skiplst = get_hetero_list(&nr_pages);

		if(!nr_pages){
			printk("send_hotpage_skiplist: get_hetero_list returns 0 pages \n");
			goto skiplisterr;
		}else {
			printk("send_hotpage_skiplist: get_hetero_list returns %u pages \n", 
								nr_pages);
		}

		for (i = 0; i < nr_pages; i++) {
				hetero_frame_list[i] =0;
		}	

		if (list_empty(hetero_skiplst)){
			printk(KERN_ALERT "send_hotpage_skiplist: nothing in the list \n");
			state = BP_EAGAIN;
			goto skiplisterr;
		}

		i=0;
		while(i < nr_pages) {

			page = list_entry(hetero_skiplst->next, struct page, nvlist);

			if(!page) {
				goto sendskiplist;
			}

			pfn = page_to_pfn(page);
			hetero_frame_list[i] = pfn_to_mfn(pfn);
			printk("Frame item i:%u mfn %x pfn %x \n", i, hetero_frame_list[i], pfn); 

			/* used_lst_pgcnt--;*/
			list_del(&page->nvlist);

			i++;
		}

sendskiplist:
		printk("number of reserved pages set for hotpage skip %u \n", nr_pages);

		/*Setting the heteromem request flag used in hypervisor*/
		reservation.mem_flags = reservation.mem_flags|XENMEMF_hetero_stop_hotpage_scan;
		set_xen_guest_handle(reservation.extent_start, hetero_frame_list);
		reservation.nr_extents = nr_pages;

		//Test XENMEM_hetero_populate_physmap call	
		ret = HYPERVISOR_memory_op(XENMEM_hetero_stop_hotpage_scan, &reservation);
		if (ret <= 0){
			//printk(KERN_DEBUG "XENMEM_hetero_stop_hotpage_scan failed %d\n", ret);
			goto skiplisterr;
		}
		return state;

skiplisterr:
		printk(KERN_ALERT "send_hotpage_skiplist: nothing in the list\n");
		return BP_EAGAIN;	
}
EXPORT_SYMBOL(send_hotpage_skiplist);



/*Check if a page is in the hotlist set by hypercall or 
guest domain*/
int is_hetero_hot_page(struct page *page){

	int idx =0;
	unsigned int hotpfn=0;
	unsigned long hotpfnlong=0;
	unsigned int pfn = page_to_pfn(page);

    if(!hetero_frame_list) {
        printk(KERN_ALERT "hetero_frame_list alloc failed \n");
        return -1;
    }

	for(idx=0; idx < hotpagecnt; idx++) {

	   hotpfn = mfn_to_pfn(hetero_frame_list[idx]);

	   if(!hotpfn) continue;

	   if( pfn == hotpfn) {
		 mfnmatchcnt++;
		 return 1;
		}
	
		if(pfn == hotpfnlong){
			//printk(KERN_ALERT "hotpfn long condition succeeds\n");
			return 1;
		}
	}
	return 0;
}
EXPORT_SYMBOL(is_hetero_hot_page);


xen_pfn_t *get_hotpage_list(unsigned int *hotcnt)
{
		enum bp_state state = BP_DONE;
		unsigned long  pfn, i;
		struct page *page = NULL;
		/*pages for which host, pfn-to-mfn table has been updated*/
		struct list_head *hetero_skiplst;
		unsigned int nr_pages;
		int ret = 0;	
	    int pfnconvfailed=0;
		struct mm_struct *mm = NULL;
		struct vm_area_struct *vma=NULL;
		unsigned long addr =0;	

		//printk("calling send_hotpage_skiplist\n");
		if(!hetero_frame_list) {
        	printk(KERN_ALERT "hetero_frame_list alloc failed \n");
        	return NULL;
    	}

		struct xen_memory_reservation reservation = {
				.address_bits = 0,
				.extent_order = 0,
				.domid        = DOMID_SELF
		};

		//printk("number of reserved pages set for hotpage skip %u \n", nr_pages);
		/*Setting the heteromem request flag used in hypervisor*/
		reservation.mem_flags = reservation.mem_flags|XENMEMF_hetero_mem_request;
		set_xen_guest_handle(reservation.extent_start, hetero_frame_list);
		reservation.nr_extents = 1;

		//Test XENMEM_hetero_populate_physmap call	
		ret = HYPERVISOR_memory_op(XENMEM_hetero_stop_hotpage_scan, &reservation);
		if (ret <= 0){
			//printk(KERN_DEBUG "XENMEM_hetero_stop_hotpage_scan failed %d\n", ret);
			//goto skiplisterr;
		}
		//printk("get_hotpage_list: XENMEM_hetero_stop_hotpage_scan "
		//				  "returns %u\n", ret);
#if 0	
		if(ret > 0) {
		 for (i = 0; i < PAGE_SIZE; i++) {

			 hetero_frame_listshadow[i]=hetero_frame_list[i];
			 hetero_frame_list[i]=0;
#if 0
			 pfnconvfailed=0;
	         pfn = mfn_to_pfn(hetero_frame_list[i]);
			 page = pfn_to_page(pfn);
			 if(!page) {
				pfnconvfailed=1;
				printk(KERN_ALERT "get_hotpage_list: page NULL \n");
			 }else {
				mm = current->mm;
				if(mm) {
        	     	addr = page_address(page);
            	 	vma = find_vma(mm, addr);
				}else {
					printk(KERN_ALERT "get_hotpage_list: mm NULL \n");
				}

			 	if(vma) {
					printk(KERN_ALERT "get_hotpage_list: vma found \n");
			 	}else {
					//printk(KERN_ALERT "get_hotpage_list: vma not found \n");
			 	}			
			 }
		 	 ///printk(KERN_ALERT "get_hotpage_list: frame_list[%d]: "
				//			   "%lu, pfn %u, pfn2page fail?%d \n",
				//				i, hetero_frame_list[i], pfn, pfnconvfailed);
		 	//printk(KERN_ALERT "get_hotpage_list: frame_list[%d]:, ret %d \n",
			//				i, ret); 
#endif
		 }
		}
#endif

		if(ret > MAX_MIGRATE)
			ret = MAX_MIGRATE;

		hotpagecnt = ret;	

		*hotcnt = ret;

		return hetero_frame_list;


#if 0
		hetero_skiplst = get_hetero_list(&nr_pages);

		if(!nr_pages){
			printk(KERN_ALERT "send_hotpage_skiplist: get_hetero_list returns 0 pages \n");
			goto skiplisterr;
		}else {
			printk(KERN_ALERT "send_hotpage_skiplist: get_hetero_list returns %u pages \n", 
								nr_pages);
		}

		for (i = 0; i < nr_pages; i++) {
				hetero_frame_list[i] =0;
		}	

		if (list_empty(hetero_skiplst)){
			printk(KERN_ALERT "send_hotpage_skiplist: nothing in the list \n");
			state = BP_EAGAIN;
			goto skiplisterr;
		}

		i=0;
		while(i < nr_pages) {

			page = list_entry(hetero_skiplst->next, struct page, nvlist);

			if(!page) {
				goto sendskiplist;
			}

			pfn = page_to_pfn(page);
			hetero_frame_list[i] = pfn_to_mfn(pfn);
			printk("Frame item i:%u mfn %x pfn %x \n", i, hetero_frame_list[i], pfn); 

			/* used_lst_pgcnt--;*/
			/*list_del(&page->nvlist);
			i++;
		}

sendskiplist:
		printk("number of reserved pages set for hotpage skip %u \n", nr_pages);
		/*Setting the heteromem request flag used in hypervisor*/
		reservation.mem_flags = reservation.mem_flags|XENMEMF_hetero_stop_hotpage_scan;
		set_xen_guest_handle(reservation.extent_start, hetero_frame_list);
		reservation.nr_extents = nr_pages;

		//Test XENMEM_hetero_populate_physmap call	
		ret = HYPERVISOR_memory_op(XENMEM_hetero_stop_hotpage_scan, &reservation);
		if (ret <= 0){
			printk(KERN_DEBUG "XENMEM_hetero_stop_hotpage_scan failed %d\n", ret);
			goto skiplisterr;
		}
		return state;

skiplisterr:
		printk(KERN_ALERT "send_hotpage_skiplist: nothing in the list\n");
		return BP_EAGAIN;	
#endif


}
EXPORT_SYMBOL(get_hotpage_list);




static enum bp_state decrease_reservation(unsigned long nr_pages, gfp_t gfp)
{
	enum bp_state state = BP_DONE;
	unsigned long  pfn, i, idx=0, iter =0, remind=0;
	struct page   *page = NULL;
	int ret;
	struct xen_memory_reservation reservation = {
		.address_bits = 0,
		.extent_order = 0,
		.domid        = DOMID_SELF
	};

	/*for (i = 0; i < nr_pages; i++) {
		if ((page = alloc_page(gfp)) == NULL) {
			nr_pages = i;
			state = BP_EAGAIN;
			break;
		}*/

	//debug_heteroused_page();
	//return BP_DONE;

	/*If the requested pages are higher than framelist size, then reserve 
	in multiple iteration. Not very optimal */
	if (nr_pages > MAX_HOT_MFN) {
        iter = (nr_pages/MAX_HOT_MFN);
		remind = nr_pages % MAX_HOT_MFN;
		if(remind){
			iter = iter + 1;
		}

		printk("attempting decrease_reservation "
				"in %lu*%lu iter* nr_pages %lu page "
				"remind %lu\n ",
				nr_pages, iter, nr_pages* iter,remind);

		nr_pages =  MAX_HOT_MFN;

	}else {
		iter = 1;
		remind = nr_pages % MAX_HOT_MFN;
	}

	for (idx =0; idx < iter; idx++) {

		 for (i = 0; i < nr_pages; i++) {
			 hetero_frame_list[i] =0;
		 }	

		//if (nr_pages > MAX_HOT_MFN)
			nr_pages = MAX_HOT_MFN;

			if((idx == iter-1) && remind)
				nr_pages = remind-3;

			printk("number of reserved pages %lu pfn of first page %lu\n", 
					nr_pages, page_to_pfn(page));

			for (i = 0; i < nr_pages; i++) {

				if(page == NULL) {
					page = heteroused_first_page();
					if(page == NULL) {
						nr_pages = i;
						state = BP_EAGAIN;
						goto start_deletion;
					}
				}
				else if ((page = heteroused_next_page(page)) == NULL) {
					nr_pages = i;
					state = BP_EAGAIN;
					goto start_deletion;
				}
				pfn = page_to_pfn(page);
				hetero_frame_list[i] = pfn_to_mfn(pfn);
			    //printk("Frame item i:%u mfn %x pfn %x \n", i, hetero_frame_list[i], pfn); 
				scrub_page(page);

		#ifdef CONFIG_XEN_HAVE_PVMMU
				if (xen_pv_domain() && !PageHighMem(page)) {
					ret = HYPERVISOR_update_va_mapping(
						(unsigned long)__va(pfn << PAGE_SHIFT),
						__pte_ma(0), 0);
					BUG_ON(ret);
				}
		#endif
			 /* used_lst_pgcnt--;*/
			  //list_del(&page->lru);
			}
start_deletion:
	
			nr_pages = i;
			printk("number of reserved pages set for deletion %lu \n", nr_pages);
			/* Ensure that heteromemed highmem pages don't have kmaps. */
			kmap_flush_unused();
			flush_tlb_all();

			/* No more mappings: invalidate P2M and add to heteromem. */
			for (i = 0; i < nr_pages; i++) {
				pfn = mfn_to_pfn(hetero_frame_list[i]);
				__set_phys_to_machine(pfn, INVALID_P2M_ENTRY);
				//heteromem_append(pfn_to_page(pfn));
			}
			set_xen_guest_handle(reservation.extent_start, hetero_frame_list);
			reservation.nr_extents   = nr_pages;
			ret = HYPERVISOR_memory_op(XENMEM_decrease_reservation, &reservation);
			printk("number of successful decreased reservation %d "
				"from target %lu pages \n", ret, nr_pages);

			BUG_ON(ret != nr_pages);
			heteromem_stats.current_pages -= nr_pages;
		}

		/*Setting the heteromem request flag used in hypervisor*/
		reservation.mem_flags = reservation.mem_flags|XENMEMF_hetero_stop_hotpage_scan;
		set_xen_guest_handle(reservation.extent_start, hetero_frame_list);
		reservation.nr_extents = nr_pages;
		//Test XENMEM_hetero_populate_physmap call	
		ret = HYPERVISOR_memory_op(XENMEM_hetero_stop_hotpage_scan, &reservation);
		if (ret <= 0){
			//printk(KERN_DEBUG "XENMEM_hetero_stop_hotpage_scan failed %d\n", ret);
			return BP_EAGAIN;
		}

	return state;
}

/*
 * We avoid multiple worker processes conflicting via the heteromem mutex.
 * We may of course race updates of the target counts (which are protected
 * by the heteromem lock), or with changes to the Xen hard limit, but we will
 * recover from these in time.
 */
static void heteromem_process(struct work_struct *work)
{
	enum bp_state state = BP_DONE;
	long credit;

	mutex_lock(&heteromem_mutex);

	do {
		credit = current_credit();

		if (credit > 0) {
			if (heteromem_is_inflated())
				state = increase_reservation(credit,NULL);
			else
				state = reserve_additional_memory(credit);
		}

		if (credit < 0)
			state = decrease_reservation(-credit, GFP_BALLOON);

		state = update_schedule(state);

#ifndef CONFIG_PREEMPT
		if (need_resched())
			schedule();
#endif
	} while (credit && state == BP_DONE);

	/* Schedule more work if there is some still to be done. */
	if (state == BP_EAGAIN)
		schedule_delayed_work(&heteromem_worker, heteromem_stats.schedule_delay * HZ);

	mutex_unlock(&heteromem_mutex);
}

/* Resets the Xen limit, sets new target, and kicks off processing. */
void heteromem_set_new_target(unsigned long target)
{
	/* No need for lock. Not read-modify-write updates. */
	heteromem_stats.target_pages = target;
	schedule_delayed_work(&heteromem_worker, 0);
}
EXPORT_SYMBOL_GPL(heteromem_set_new_target);

/**
 * alloc_xenheteromemed_pages - get pages that have been heteromemed out
 * @nr_pages: Number of pages to get
 * @pages: pages returned
 * @highmem: allow highmem pages
 * @return 0 on success, error otherwise
 */
int alloc_xenheteromemed_pages(int nr_pages, struct page **pages, bool highmem, int delpage)
{
	int pgno = 0, idx, iter=0;
	//struct page *page;
	enum bp_state st = BP_EAGAIN;
	unsigned int remind =0;

	mutex_lock(&heteromem_mutex);

#if 0
	while (pgno < nr_pages) {
		page = heteromem_retrieve(highmem);
		if (page && (highmem || !PageHighMem(page))) {
			pages[pgno++] = page;
		} else {
			enum bp_state st;
			if (page)
				heteromem_append(page);
			st = decrease_reservation(nr_pages - pgno,
					highmem ? GFP_HIGHUSER : GFP_USER);
			if (st != BP_DONE)
				goto out_undo;
		}
	}
	if(delpage) {
		nr_pages = used_lst_pgcnt;
		if (decrease_reservation(nr_pages, (gfp_t)1) != BP_DONE) {
			printk("decrease_reservation failed in iteration %u \n");
			goto out_undo;
		}
		goto unlock;
	}
#endif

	printk("alloc_xenheteromemed_pages called \n");

	if (delpage) {
		 nr_pages = used_lst_pgcnt;
		if (decrease_reservation(nr_pages, (gfp_t)1) != BP_DONE) {
			printk("decrease_reservation failed in iteration\n");
			goto out_undo;
		}
		
	}else {

	/*If the requested pages are higher than framelist size, then reserve 
	in multiple iteration. Not very optimal */
		if (nr_pages > MAX_HOT_MFN) {

			iter = (nr_pages/MAX_HOT_MFN);
			remind = nr_pages % MAX_HOT_MFN;
			if(remind) {
				iter = iter + 1;
			}
			nr_pages =  MAX_HOT_MFN;
		}else {
			iter = 1;
		}

		for (idx =0; idx < iter; idx++) {
			st = increase_reservation(nr_pages,pages);	
			if(st != BP_DONE){
				printk("heteromem reservation succeeded only for %u of %u pages \n",
				idx*nr_pages, iter*nr_pages);
				goto out_undo;	
			}
		}
	}
	printk("heteromem reservation succeeded for %u  pages \n",
					iter*nr_pages);

	mutex_unlock(&heteromem_mutex);
	return 0;

 out_undo:
	//while (pgno)
		//heteromem_append(pages[--pgno]);

	/* Free the memory back to the kernel soon */
	//schedule_delayed_work(&heteromem_worker, 0);

	mutex_unlock(&heteromem_mutex);
	return -ENOMEM;
}
EXPORT_SYMBOL(alloc_xenheteromemed_pages);

/**
 * free_xenheteromemed_pages - return pages retrieved with get_heteromemed_pages
 * @nr_pages: Number of pages
 * @pages: pages to return
 */
void free_xenheteromemed_pages(int nr_pages, struct page **pages)
{
	int i;

	mutex_lock(&heteromem_mutex);

	for (i = 0; i < nr_pages; i++) {
		if (pages[i])
			heteromem_append(pages[i]);
	}

	/* The heteromem may be too large now. Shrink it if needed. */
	if (current_credit())
		schedule_delayed_work(&heteromem_worker, 0);

	mutex_unlock(&heteromem_mutex);
}
EXPORT_SYMBOL(free_xenheteromemed_pages);

int debugflg=0;

void debug_heteroused_page(void)
{

	struct page *debug_page;
	struct list_head *next;
	int idx=0;

	if (list_empty(&hetero_used_lst_pgs))
		return;

	debug_page = list_entry(hetero_used_lst_pgs.next, struct page, lru);

	for(idx =0; idx < used_lst_pgcnt; idx++) {
	//while(debug_page) {

		 if(debug_page)
		 	printk(" next pfn: %lu, mfn :%lu \n",
			 page_to_pfn(debug_page), pfn_to_mfn(page_to_pfn(debug_page)));

		next = debug_page->lru.next;
		if (next == &hetero_used_lst_pgs)
			return;

		 debug_page = list_entry(next, struct page, lru);
	}

}




/* heteromem_retrieve: rescue a page from the heteromem, if it is not empty. */
struct page *hetero_getnxt_page(bool prefer_highmem)
{
	struct page *page;
	unsigned long pfn;
	

	if (list_empty(&hetero_ready_lst_pgs)){
		printk(KERN_DEBUG "hetero_getnxt_page: list is empty: %u \n",
				ready_lst_pgcnt);
		return NULL;
	}
	if(!prefer_highmem)
		page = list_entry(hetero_ready_lst_pgs.next, struct page, lru);
	else
		page = list_entry(hetero_ready_lst_pgs.prev, struct page, lru);

    if(!page) {
		printk(KERN_DEBUG "hetero_getnxt_page: list is empty \n");
		return NULL;
	}
	list_del(&page->lru);

	if(ready_lst_pgcnt)
		ready_lst_pgcnt--;

	pfn= page_to_pfn(page);
	//printk("hetero_getnxt_page: Frame mfn %x pfn %x \n", pfn_to_mfn(pfn), page_to_pfn(page)); 

	//add to used list of pages
	list_add(&page->lru, &hetero_used_lst_pgs);
	init_page_count(page);
	used_lst_pgcnt++;

	//if(used_lst_pgcnt == 2048)
		//debug_heteroused_page();

	
#ifdef HETERODEBUG
	printk(KERN_DEBUG "getting page from hetero ready list %lu "
			"ready_lst_pgcnt %u reserv hetro pgs %u\n", 
			page_to_pfn(page), ready_lst_pgcnt, dbg_resv_hetropg_cnt);
#endif
	return page;
}
EXPORT_SYMBOL(hetero_getnxt_page);



static void __init heteromem_add_region(unsigned long start_pfn,
				      unsigned long pages)
{
	unsigned long pfn, extra_pfn_end;
	struct page *page;

	/*
	 * If the amount of usable memory has been limited (e.g., with
	 * the 'mem' command line parameter), don't add pages beyond
	 * this limit.
	 */
	printk(KERN_DEBUG "in heteromem_add_region...\n");
	extra_pfn_end = min(max_pfn, start_pfn + pages);
	printk(KERN_DEBUG "heteromem_add_region maxpfn %lu, "
			   " start_pfn + pages %lu extra_pfn_end %lu\n",
			   max_pfn, start_pfn + pages, extra_pfn_end);

	for (pfn = start_pfn; pfn < extra_pfn_end; pfn++) {
		page = pfn_to_page(pfn);
		/* totalram_pages and totalhigh_pages do not
		   include the boot-time heteromem extension, so
		   don't subtract from it. */
		__heteromem_append(page);
	}
	printk(KERN_DEBUG "heteromem_add_region: appended %lu pages "
					  "heteromem_high pages %lu, "
					  "heteromem_low pages %lu \n",
						extra_pfn_end - start_pfn,
						heteromem_stats.heteromem_high++, 
						heteromem_stats.heteromem_low++);
}

int heteromem_init(int idx, unsigned long start, unsigned long size)
{
	if (!xen_domain())
		return -ENODEV;

	pr_info("xen/heteromem: Initialising heteromem driver.\n");

	/*heteromem_stats.current_pages = xen_pv_domain()
		? min(xen_start_info->nr_pages - xen_released_pages, max_pfn)
		: max_pfn;
	heteromem_stats.target_pages  = heteromem_stats.current_pages;
	heteromem_stats.heteromem_low   = 0;
	heteromem_stats.heteromem_high  = 0;

	heteromem_stats.schedule_delay = 1;
	heteromem_stats.max_schedule_delay = 32;
	heteromem_stats.retry_count = 1;
	heteromem_stats.max_retry_count = RETRY_UNLIMITED;*/

	printk(KERN_DEBUG "heteromem: Adding extramem[%d]:%lu, start %lu\n", 
			idx, size,start);

	if (size){
		pr_info("xen/heteromem: calling heteromem_add_region\n");
		heteromem_add_region(PFN_UP(start),
				   PFN_DOWN(size));
	}
	
	/*hetero_frame_list =  kmalloc(MAX_HOT_MFN, GFP_KERNEL);
	if(!hetero_frame_list) {
		printk(KERN_ALERT "hetero_frame_list alloc failed \n");
	}*/	

	return 0;
}
EXPORT_SYMBOL(heteromem_init);




/* DONT NEED HETEROMEM AS A DRIVER FOR NOW*/
#if 0
static int __init heteromem_init(void)
{
	int i;

	if (!xen_domain())
		return -ENODEV;

	pr_info("xen/heteromem: Initialising heteromem driver.\n");

	heteromem_stats.current_pages = xen_pv_domain()
		? min(xen_start_info->nr_pages - xen_released_pages, max_pfn)
		: max_pfn;
	heteromem_stats.target_pages  = heteromem_stats.current_pages;
	heteromem_stats.heteromem_low   = 0;
	heteromem_stats.heteromem_high  = 0;

	heteromem_stats.schedule_delay = 1;
	heteromem_stats.max_schedule_delay = 32;
	heteromem_stats.retry_count = 1;
	heteromem_stats.max_retry_count = RETRY_UNLIMITED;

	/*
	 * Initialize the heteromem with pages from the extra memory
	 * regions (see arch/x86/xen/setup.c).
	 */
	for (i = 0; i < XEN_EXTRA_MEM_MAX_REGIONS; i++){

		printk(KERN_DEBUG "heteromem: Adding extramem[%d]:%u \n", i,xen_extra_mem[i].size);

		if (xen_extra_mem[i].size)
			heteromem_add_region(PFN_UP(xen_extra_mem[i].start),
					   PFN_DOWN(xen_extra_mem[i].size));
	}

	return 0;
}

subsys_initcall(heteromem_init);

MODULE_LICENSE("GPL");
#endif
