/*
 * Memory Migration functionality - linux/mm/migration.c
 *
 * Copyright (C) 2006 Silicon Graphics, Inc., Christoph Lameter
 *
 * Page migration was first developed in the context of the memory hotplug
 * project. The main authors of the migration code are:
 *
 * IWAMOTO Toshihiro <iwamoto@valinux.co.jp>
 * Hirokazu Takahashi <taka@valinux.co.jp>
 * Dave Hansen <haveblue@us.ibm.com>
 * Christoph Lameter
 */

#include <linux/migrate.h>
#include <linux/export.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/pagemap.h>
#include <linux/buffer_head.h>
#include <linux/mm_inline.h>
#include <linux/nsproxy.h>
#include <linux/pagevec.h>
#include <linux/ksm.h>
#include <linux/rmap.h>
#include <linux/topology.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/writeback.h>
#include <linux/mempolicy.h>
#include <linux/vmalloc.h>
#include <linux/security.h>
#include <linux/memcontrol.h>
#include <linux/syscalls.h>
#include <linux/hugetlb.h>
#include <linux/hugetlb_cgroup.h>
#include <linux/gfp.h>
#include <linux/balloon_compaction.h>
#include <linux/mm.h>
#include <linux/mempolicy.h>
#include <asm/tlbflush.h>

#include <xen/balloon.h>
#include <xen/heteromem.h>
#include <asm/xen/page.h>
#include <xen/amaro.h>
#include <asm/page.h>
#include <asm/io.h>


#define CREATE_TRACE_POINTS
#include <trace/events/migrate.h>
#include "internal.h"

#define ENABLE_HETERO
#define HETERO_SKIPLIST_LEN 1024*512
//#define SELECTIVE_HETERO_SCAN
#define HETERO_PRINT_STATS 0
#define HETERO_APP_INIT 10
#define HOT_MIN_MIG_LIMIT 8
//#define DEBUG_TIMER
//#define HETEROSTATS
#define _DISABLE_HETERO_CHECK
#define HETEROFAIL -1
//#define ENABLE_OS_MIGRATION

unsigned int g_PG_swapbacked;
unsigned int g_PG_lru;
unsigned int g_PG_active;
unsigned int g_PG_inactive;
unsigned int g_PG_dirty;
unsigned int g_PG_private;
unsigned int g_PG_writeback;
unsigned int g_PG_reserved;


#ifdef DEBUG_TIMER
unsigned long tot_mig_time;
unsigned long tot_pgwalk_time;
unsigned long tot_hypercall_time;
#endif

#ifdef HETEROSTATS
#define MAXVMA 40
unsigned long vma_list[MAXVMA];
unsigned long vma_hotpgcnt[MAXVMA];
unsigned int vmacntr;
static unsigned int num_migrated;
/*Xen reported hot pages*/
static unsigned int stat_xen_hot_pages;
static unsigned int stat_tot_proc_pages;
#endif

/* Internal flags */
#define MPOL_MF_DISCONTIG_OK (MPOL_MF_INTERNAL << 0)    /* Skip checks for continuous vmas */
#define MPOL_MF_INVERT (MPOL_MF_INTERNAL << 1)          /* Invert check for nodemask */


#ifdef ENABLE_HETERO
struct list_head hetero_list;
unsigned int nr_heteropgcnt;
int init_hetero_list;
void init_hetero_list_fn();

static struct page *new_slowpage(
		struct page *p, 
		unsigned long private, 
		int **result);

#endif
//#define HETERODEBUG
unsigned int nr_migrate_success;
unsigned int nr_curr_migrate_success;
unsigned int nr_reclaim_success;
unsigned int nr_migrate_attempted;
unsigned int nr_page_freed;
unsigned int nr_alloc_fail;

unsigned int pg_debug_count;
unsigned int pg_busy;
unsigned int pg_rtnode_err;
unsigned int pg_notexist;
unsigned int pg_nopte;

unsigned int beforemigrate_dbg_count;
unsigned int nonrsrvpg_dbg_count;
unsigned int  normalpg_dbg_count;
unsigned int num_unmap_pages;
unsigned int num_pages_before_move;
unsigned int nr_migrate_retry;
unsigned int nr_migrate_failed;
unsigned int nr_dup_hot_page;
unsigned int nr_invalid_page;
unsigned int nr_incorrect_pg;

unsigned int pg_debug_count;
unsigned int pageinhotlist;
unsigned long debug_pfn[4096];
unsigned long debug_pfn_cnt[4096];

spinlock_t *migrate_lock;
spinlock_t *migrate_slowmem_lock;


#ifdef HETEROSTATS
int do_complete_page_walk();
#endif

/*
 * migrate_prep() needs to be called before we start compiling a list of pages
 * to be migrated using isolate_lru_page(). If scheduling work on other CPUs is
 * undesirable, use migrate_prep_local()
 */
int migrate_prep(void)
{
	/*
	 * Clear the LRU lists so pages can be isolated.
	 * Note that pages may be moved off the LRU after we have
	 * drained them. Those pages will fail to migrate like other
	 * pages that may be busy.
	 */
	lru_add_drain_all();

	return 0;
}

/* Do the necessary work of migrate_prep but not if it involves other CPUs */
int migrate_prep_local(void)
{
	lru_add_drain();

	return 0;
}

/*
 * Add isolated pages on the list back to the LRU under page lock
 * to avoid leaking evictable pages back onto unevictable list.
 */
void putback_lru_pages(struct list_head *l)
{
	struct page *page;
	struct page *page2;

	list_for_each_entry_safe(page, page2, l, lru) {
		list_del(&page->lru);
		dec_zone_page_state(page, NR_ISOLATED_ANON +
				page_is_file_cache(page));
			putback_lru_page(page);
	}
}

/*
 * Put previously isolated pages back onto the appropriate lists
 * from where they were once taken off for compaction/migration.
 *
 * This function shall be used instead of putback_lru_pages(),
 * whenever the isolated pageset has been built by isolate_migratepages_range()
 */
void putback_movable_pages(struct list_head *l)
{
	struct page *page;
	struct page *page2;

	list_for_each_entry_safe(page, page2, l, lru) {
		list_del(&page->lru);
		dec_zone_page_state(page, NR_ISOLATED_ANON +
				page_is_file_cache(page));
		if (unlikely(balloon_page_movable(page)))
			balloon_page_putback(page);
		else
			putback_lru_page(page);
	}
}

/*
 * Restore a potential migration pte to a working pte entry
 */
static int remove_migration_pte(struct page *new, struct vm_area_struct *vma,
				 unsigned long addr, void *old)
{
	struct mm_struct *mm = vma->vm_mm;
	swp_entry_t entry;
 	pmd_t *pmd;
	pte_t *ptep, pte;
 	spinlock_t *ptl;

	if (unlikely(PageHuge(new))) {
		ptep = huge_pte_offset(mm, addr);
		if (!ptep)
			goto out;
		ptl = &mm->page_table_lock;
	} else {
		pmd = mm_find_pmd(mm, addr);
		if (!pmd)
			goto out;
		if (pmd_trans_huge(*pmd))
			goto out;

		ptep = pte_offset_map(pmd, addr);

		/*
		 * Peek to check is_swap_pte() before taking ptlock?  No, we
		 * can race mremap's move_ptes(), which skips anon_vma lock.
		 */

		ptl = pte_lockptr(mm, pmd);
	}

 	spin_lock(ptl);
	pte = *ptep;
	if (!is_swap_pte(pte))
		goto unlock;

	entry = pte_to_swp_entry(pte);

	if (!is_migration_entry(entry) ||
	    migration_entry_to_page(entry) != old)
		goto unlock;

	get_page(new);
	pte = pte_mkold(mk_pte(new, vma->vm_page_prot));
	if (is_write_migration_entry(entry))
		pte = pte_mkwrite(pte);
#ifdef CONFIG_HUGETLB_PAGE
	if (PageHuge(new)) {
		pte = pte_mkhuge(pte);
		pte = arch_make_huge_pte(pte, vma, new, 0);
	}
#endif
	flush_cache_page(vma, addr, pte_pfn(pte));
	set_pte_at(mm, addr, ptep, pte);

	if (PageHuge(new)) {
		if (PageAnon(new))
			hugepage_add_anon_rmap(new, vma, addr);
		else
			page_dup_rmap(new);
	} else if (PageAnon(new))
		page_add_anon_rmap(new, vma, addr);
	else
		page_add_file_rmap(new);

	/* No need to invalidate - it was non-present before */
	update_mmu_cache(vma, addr, ptep);
unlock:
	pte_unmap_unlock(ptep, ptl);
out:
	return SWAP_AGAIN;
}

/*
 * Get rid of all migration entries and replace them by
 * references to the indicated page.
 */
static void remove_migration_ptes(struct page *old, struct page *new)
{
	int ret = 0;
	int page_was_unlocked=0;
	int page2_was_unlocked=0;


  if(new  && !PageLocked(new)){
        lock_page(new);
        page_was_unlocked = 1;
    }

#if 0
   if(old  && !PageLocked(old)){
        lock_page(old);
        page2_was_unlocked = 1;
    }
#endif
	ret = rmap_walk(new, remove_migration_pte, old);

	if(page_was_unlocked)
            unlock_page(new);

#if 0
	if(page2_was_unlocked)
            unlock_page(old);
#endif
	return ret;
}

/*
 * Something used the pte of a page under migration. We need to
 * get to the page and wait until migration is finished.
 * When we return from this function the fault will be retried.
 */
void migration_entry_wait(struct mm_struct *mm, pmd_t *pmd,
				unsigned long address)
{
	pte_t *ptep, pte;
	spinlock_t *ptl;
	swp_entry_t entry;
	struct page *page;

	ptep = pte_offset_map_lock(mm, pmd, address, &ptl);
	pte = *ptep;
	if (!is_swap_pte(pte))
		goto out;

	entry = pte_to_swp_entry(pte);
	if (!is_migration_entry(entry))
		goto out;

	page = migration_entry_to_page(entry);

	/*
	 * Once radix-tree replacement of page migration started, page_count
	 * *must* be zero. And, we don't want to call wait_on_page_locked()
	 * against a page without get_page().
	 * So, we use get_page_unless_zero(), here. Even failed, page fault
	 * will occur again.
	 */
	if (!get_page_unless_zero(page))
		goto out;
	pte_unmap_unlock(ptep, ptl);
	wait_on_page_locked(page);
	put_page(page);
	return;
out:
	pte_unmap_unlock(ptep, ptl);
}

#ifdef CONFIG_BLOCK
/* Returns true if all buffers are successfully locked */
static bool buffer_migrate_lock_buffers(struct buffer_head *head,
							enum migrate_mode mode)
{
	struct buffer_head *bh = head;

	/* Simple case, sync compaction */
	if (mode != MIGRATE_ASYNC) {
		do {
			get_bh(bh);
			lock_buffer(bh);
			bh = bh->b_this_page;

		} while (bh != head);

		return true;
	}

	/* async case, we cannot block on lock_buffer so use trylock_buffer */
	do {
		get_bh(bh);
		if (!trylock_buffer(bh)) {
			/*
			 * We failed to lock the buffer and cannot stall in
			 * async migration. Release the taken locks
			 */
			struct buffer_head *failed_bh = bh;
			put_bh(failed_bh);
			bh = head;
			while (bh != failed_bh) {
				unlock_buffer(bh);
				put_bh(bh);
				bh = bh->b_this_page;
			}
			return false;
		}

		bh = bh->b_this_page;
	} while (bh != head);
	return true;
}
#else
static inline bool buffer_migrate_lock_buffers(struct buffer_head *head,
							enum migrate_mode mode)
{
	return true;
}
#endif /* CONFIG_BLOCK */

/*
 * Replace the page in the mapping.
 *
 * The number of remaining references must be:
 * 1 for anonymous pages without a mapping
 * 2 for pages with a mapping
 * 3 for pages with a mapping and PagePrivate/PagePrivate2 set.
 */
static int migrate_page_move_mapping(struct address_space *mapping,
		struct page *newpage, struct page *page,
		struct buffer_head *head, enum migrate_mode mode)
{
	int expected_count = 0;
	void **pslot;

	if (!mapping) {
		/* Anonymous page without mapping */
		if (page_count(page) != 1)
			return -EAGAIN;
		return MIGRATEPAGE_SUCCESS;
	}

	spin_lock_irq(&mapping->tree_lock);

	pslot = radix_tree_lookup_slot(&mapping->page_tree,
 					page_index(page));

	expected_count = 2 + page_has_private(page);
	if (page_count(page) != expected_count ||
		radix_tree_deref_slot_protected(pslot, &mapping->tree_lock) != page) {
		spin_unlock_irq(&mapping->tree_lock);
		return -EAGAIN;
	}

	if (!page_freeze_refs(page, expected_count)) {
		spin_unlock_irq(&mapping->tree_lock);
		return -EAGAIN;
	}

	/*
	 * In the async migration case of moving a page with buffers, lock the
	 * buffers using trylock before the mapping is moved. If the mapping
	 * was moved, we later failed to lock the buffers and could not move
	 * the mapping back due to an elevated page count, we would have to
	 * block waiting on other references to be dropped.
	 */
	if (mode == MIGRATE_ASYNC && head &&
			!buffer_migrate_lock_buffers(head, mode)) {
		page_unfreeze_refs(page, expected_count);
		spin_unlock_irq(&mapping->tree_lock);
		return -EAGAIN;
	}

	/*
	 * Now we know that no one else is looking at the page.
	 */
	get_page(newpage);	/* add cache reference */
	if (PageSwapCache(page)) {
		SetPageSwapCache(newpage);
		set_page_private(newpage, page_private(page));
	}

	radix_tree_replace_slot(pslot, newpage);

	/*
	 * Drop cache reference from old page by unfreezing
	 * to one less reference.
	 * We know this isn't the last reference.
	 */
	page_unfreeze_refs(page, expected_count - 1);

	/*
	 * If moved to a different zone then also account
	 * the page for that zone. Other VM counters will be
	 * taken care of when we establish references to the
	 * new page and drop references to the old page.
	 *
	 * Note that anonymous pages are accounted for
	 * via NR_FILE_PAGES and NR_ANON_PAGES if they
	 * are mapped to swap space.
	 */
	__dec_zone_page_state(page, NR_FILE_PAGES);
	__inc_zone_page_state(newpage, NR_FILE_PAGES);
	if (!PageSwapCache(page) && PageSwapBacked(page)) {
		__dec_zone_page_state(page, NR_SHMEM);
		__inc_zone_page_state(newpage, NR_SHMEM);
	}
	spin_unlock_irq(&mapping->tree_lock);

	return MIGRATEPAGE_SUCCESS;
}

/*
 * The expected number of remaining references is the same as that
 * of migrate_page_move_mapping().
 */
int migrate_huge_page_move_mapping(struct address_space *mapping,
				   struct page *newpage, struct page *page)
{
	int expected_count;
	void **pslot;

	if (!mapping) {
		if (page_count(page) != 1)
			return -EAGAIN;
		return MIGRATEPAGE_SUCCESS;
	}

	spin_lock_irq(&mapping->tree_lock);

	pslot = radix_tree_lookup_slot(&mapping->page_tree,
					page_index(page));

	expected_count = 2 + page_has_private(page);
	if (page_count(page) != expected_count ||
		radix_tree_deref_slot_protected(pslot, &mapping->tree_lock) != page) {
		spin_unlock_irq(&mapping->tree_lock);
		return -EAGAIN;
	}

	if (!page_freeze_refs(page, expected_count)) {
		spin_unlock_irq(&mapping->tree_lock);
		return -EAGAIN;
	}

	get_page(newpage);

	radix_tree_replace_slot(pslot, newpage);

	page_unfreeze_refs(page, expected_count - 1);

	spin_unlock_irq(&mapping->tree_lock);
	return MIGRATEPAGE_SUCCESS;
}

/*
 * Copy the page to its new location
 */
void migrate_page_copy(struct page *newpage, struct page *page)
{
	if (PageHuge(page) || PageTransHuge(page))
		copy_huge_page(newpage, page);
	else
		copy_highpage(newpage, page);

	if (PageError(page))
		SetPageError(newpage);
	if (PageReferenced(page))
		SetPageReferenced(newpage);
	if (PageUptodate(page))
		SetPageUptodate(newpage);
	if (TestClearPageActive(page)) {
		VM_BUG_ON(PageUnevictable(page));
		SetPageActive(newpage);
	} else if (TestClearPageUnevictable(page))
		SetPageUnevictable(newpage);
	if (PageChecked(page))
		SetPageChecked(newpage);
	if (PageMappedToDisk(page))
		SetPageMappedToDisk(newpage);

	if (PageDirty(page)) {
		clear_page_dirty_for_io(page);
		/*
		 * Want to mark the page and the radix tree as dirty, and
		 * redo the accounting that clear_page_dirty_for_io undid,
		 * but we can't use set_page_dirty because that function
		 * is actually a signal that all of the page has become dirty.
		 * Whereas only part of our page may be dirty.
		 */
		if (PageSwapBacked(page))
			SetPageDirty(newpage);
		else
			__set_page_dirty_nobuffers(newpage);
 	}

	mlock_migrate_page(newpage, page);
	ksm_migrate_page(newpage, page);
	/*
	 * Please do not reorder this without considering how mm/ksm.c's
	 * get_ksm_page() depends upon ksm_migrate_page() and PageSwapCache().
	 */
	ClearPageSwapCache(page);
	ClearPagePrivate(page);
	set_page_private(page, 0);

	/*
	 * If any waiters have accumulated on the new page then
	 * wake them up.
	 */
	if (PageWriteback(newpage))
		end_page_writeback(newpage);
}

/************************************************************
 *                    Migration functions
 ***********************************************************/

/* Always fail migration. Used for mappings that are not movable */
int fail_migrate_page(struct address_space *mapping,
			struct page *newpage, struct page *page)
{
	return -EIO;
}
EXPORT_SYMBOL(fail_migrate_page);

/*
 * Common logic to directly migrate a single page suitable for
 * pages that do not use PagePrivate/PagePrivate2.
 *
 * Pages are locked upon entry and exit.
 */
int migrate_page(struct address_space *mapping,
		struct page *newpage, struct page *page,
		enum migrate_mode mode)
{
	int rc;

	BUG_ON(PageWriteback(page));	/* Writeback must be complete */

	rc = migrate_page_move_mapping(mapping, newpage, page, NULL, mode);

	if (rc != MIGRATEPAGE_SUCCESS)
		return rc;

	migrate_page_copy(newpage, page);
	return MIGRATEPAGE_SUCCESS;
}
EXPORT_SYMBOL(migrate_page);

#ifdef CONFIG_BLOCK
/*
 * Migration function for pages with buffers. This function can only be used
 * if the underlying filesystem guarantees that no other references to "page"
 * exist.
 */
int buffer_migrate_page(struct address_space *mapping,
		struct page *newpage, struct page *page, enum migrate_mode mode)
{
	struct buffer_head *bh, *head;
	int rc;

	if (!page_has_buffers(page))
		return migrate_page(mapping, newpage, page, mode);

	head = page_buffers(page);

	rc = migrate_page_move_mapping(mapping, newpage, page, head, mode);

	if (rc != MIGRATEPAGE_SUCCESS)
		return rc;

	/*
	 * In the async case, migrate_page_move_mapping locked the buffers
	 * with an IRQ-safe spinlock held. In the sync case, the buffers
	 * need to be locked now
	 */
	if (mode != MIGRATE_ASYNC)
		BUG_ON(!buffer_migrate_lock_buffers(head, mode));

	ClearPagePrivate(page);
	set_page_private(newpage, page_private(page));
	set_page_private(page, 0);
	put_page(page);
	get_page(newpage);

	bh = head;
	do {
		set_bh_page(bh, newpage, bh_offset(bh));
		bh = bh->b_this_page;

	} while (bh != head);

	SetPagePrivate(newpage);

	migrate_page_copy(newpage, page);

	bh = head;
	do {
		unlock_buffer(bh);
 		put_bh(bh);
		bh = bh->b_this_page;

	} while (bh != head);

	return MIGRATEPAGE_SUCCESS;
}
EXPORT_SYMBOL(buffer_migrate_page);
#endif

/*
 * Writeback a page to clean the dirty state
 */
static int writeout(struct address_space *mapping, struct page *page)
{
	struct writeback_control wbc = {
		.sync_mode = WB_SYNC_NONE,
		.nr_to_write = 1,
		.range_start = 0,
		.range_end = LLONG_MAX,
		.for_reclaim = 1
	};
	int rc;

	if (!mapping->a_ops->writepage)
		/* No write method for the address space */
		return -EINVAL;

	if (!clear_page_dirty_for_io(page))
		/* Someone else already triggered a write */
		return -EAGAIN;

	/*
	 * A dirty page may imply that the underlying filesystem has
	 * the page on some queue. So the page must be clean for
	 * migration. Writeout may mean we loose the lock and the
	 * page state is no longer what we checked for earlier.
	 * At this point we know that the migration attempt cannot
	 * be successful.
	 */
	remove_migration_ptes(page, page);

	rc = mapping->a_ops->writepage(page, &wbc);

	if (rc != AOP_WRITEPAGE_ACTIVATE)
		/* unlocked. Relock */
		lock_page(page);

	return (rc < 0) ? -EIO : -EAGAIN;
}

/*
 * Default handling if a filesystem does not provide a migration function.
 */
static int fallback_migrate_page(struct address_space *mapping,
	struct page *newpage, struct page *page, enum migrate_mode mode)
{
	if (PageDirty(page)) {
		/* Only writeback pages in full synchronous migration */
		if (mode != MIGRATE_SYNC)
			return -EBUSY;
		return writeout(mapping, page);
	}

	/*
	 * Buffers may be managed in a filesystem specific way.
	 * We must have no buffers or drop them.
	 */
	if (page_has_private(page) &&
	    !try_to_release_page(page, GFP_KERNEL))
		return -EAGAIN;

	return migrate_page(mapping, newpage, page, mode);
}

/*
 * Move a page to a newly allocated page
 * The page is locked and all ptes have been successfully removed.
 *
 * The new page will have replaced the old page if this function
 * is successful.
 *
 * Return value:
 *   < 0 - error code
 *  MIGRATEPAGE_SUCCESS - success
 */
static int move_to_new_page(struct page *newpage, struct page *page,
				int remap_swapcache, enum migrate_mode mode)
{
	struct address_space *mapping;
	int rc;

	/*
	 * Block others from accessing the page when we get around to
	 * establishing additional references. We are the only one
	 * holding a reference to the new page at this point.
	 */
	if (!trylock_page(newpage))
		BUG();

	/* Prepare mapping for the new page.*/
	newpage->index = page->index;
	newpage->mapping = page->mapping;
	if (PageSwapBacked(page))
		SetPageSwapBacked(newpage);

	mapping = page_mapping(page);
	if (!mapping)
		rc = migrate_page(mapping, newpage, page, mode);
	else if (mapping->a_ops->migratepage)
		/*
		 * Most pages have a mapping and most filesystems provide a
		 * migratepage callback. Anonymous pages are part of swap
		 * space which also has its own migratepage callback. This
		 * is the most common path for page migration.
		 */
		rc = mapping->a_ops->migratepage(mapping,
						newpage, page, mode);
	else
		rc = fallback_migrate_page(mapping, newpage, page, mode);

	if (rc != MIGRATEPAGE_SUCCESS) {
		newpage->mapping = NULL;
	} else {
		if (remap_swapcache)
			remove_migration_ptes(page, newpage);
		page->mapping = NULL;
	}

	unlock_page(newpage);

	return rc;
}

static int __unmap_and_move(struct page *page, struct page *newpage,
				int force, enum migrate_mode mode)
{
	int rc = -EAGAIN;
	int remap_swapcache = 1;
	struct mem_cgroup *mem;
	struct anon_vma *anon_vma = NULL;

	//dump_page(page);
	num_unmap_pages++;
	

	if (!trylock_page(page)) {
		if (!force || mode == MIGRATE_ASYNC)
			goto out;

		/*
		 * It's not safe for direct compaction to call lock_page.
		 * For example, during page readahead pages are added locked
		 * to the LRU. Later, when the IO completes the pages are
		 * marked uptodate and unlocked. However, the queueing
		 * could be merging multiple pages for one bio (e.g.
		 * mpage_readpages). If an allocation happens for the
		 * second or third page, the process can end up locking
		 * the same page twice and deadlocking. Rather than
		 * trying to be clever about what pages can be locked,
		 * avoid the use of lock_page for direct compaction
		 * altogether.
		 */
		if (current->flags & PF_MEMALLOC)
			goto out;

		lock_page(page);
	}

	/* charge against new page */
	mem_cgroup_prepare_migration(page, newpage, &mem);

	if (PageWriteback(page)) {
		/*
		 * Only in the case of a full syncronous migration is it
		 * necessary to wait for PageWriteback. In the async case,
		 * the retry loop is too short and in the sync-light case,
		 * the overhead of stalling is too much
		 */
		if (mode != MIGRATE_SYNC) {
			rc = -EBUSY;
			goto uncharge;
		}
		if (!force)
			goto uncharge;
		wait_on_page_writeback(page);
	}
	/*
	 * By try_to_unmap(), page->mapcount goes down to 0 here. In this case,
	 * we cannot notice that anon_vma is freed while we migrates a page.
	 * This get_anon_vma() delays freeing anon_vma pointer until the end
	 * of migration. File cache pages are no problem because of page_lock()
	 * File Caches may use write_page() or lock_page() in migration, then,
	 * just care Anon page here.
	 */
	if (PageAnon(page) && !PageKsm(page)) {
		/*
		 * Only page_lock_anon_vma_read() understands the subtleties of
		 * getting a hold on an anon_vma from outside one of its mms.
		 */
		anon_vma = page_get_anon_vma(page);
		if (anon_vma) {
			/*
			 * Anon page
			 */
		} else if (PageSwapCache(page)) {
			/*
			 * We cannot be sure that the anon_vma of an unmapped
			 * swapcache page is safe to use because we don't
			 * know in advance if the VMA that this page belonged
			 * to still exists. If the VMA and others sharing the
			 * data have been freed, then the anon_vma could
			 * already be invalid.
			 *
			 * To avoid this possibility, swapcache pages get
			 * migrated but are not remapped when migration
			 * completes
			 */
			remap_swapcache = 0;
		} else {
			goto uncharge;
		}
	}

	if (unlikely(balloon_page_movable(page))) {
		/*
		 * A ballooned page does not need any special attention from
		 * physical to virtual reverse mapping procedures.
		 * Skip any attempt to unmap PTEs or to remap swap cache,
		 * in order to avoid burning cycles at rmap level, and perform
		 * the page migration right away (proteced by page lock).
		 */
		rc = balloon_page_migrate(newpage, page, mode);
		printk(KERN_ALERT "movepage: Balloon page \n");
		goto uncharge;
	}

	/*
	 * Corner case handling:
	 * 1. When a new swap-cache page is read into, it is added to the LRU
	 * and treated as swapcache but it has no rmap yet.
	 * Calling try_to_unmap() against a page->mapping==NULL page will
	 * trigger a BUG.  So handle it here.
	 * 2. An orphaned page (see truncate_complete_page) might have
	 * fs-private metadata. The page can be picked up due to memory
	 * offlining.  Everywhere else except page reclaim, the page is
	 * invisible to the vm, so the page can not be migrated.  So try to
	 * free the metadata, so the page can be freed.
	 */
	if (!page->mapping) {
		VM_BUG_ON(PageAnon(page));
		if (page_has_private(page)) {
			try_to_free_buffers(page);
			goto uncharge;
		}
		goto skip_unmap;
	}

	/* Establish migration ptes or remove ptes */
	try_to_unmap(page, TTU_MIGRATION|TTU_IGNORE_MLOCK|TTU_IGNORE_ACCESS);

skip_unmap:

	num_pages_before_move++;

	if (!page_mapped(page))
		rc = move_to_new_page(newpage, page, remap_swapcache, mode);

	if (rc && remap_swapcache)
		remove_migration_ptes(page, page);

	/* Drop an anon_vma reference if we took one */
	if (anon_vma)
		put_anon_vma(anon_vma);

uncharge:
	mem_cgroup_end_migration(mem, page, newpage,
				 (rc == MIGRATEPAGE_SUCCESS ||
				  rc == MIGRATEPAGE_BALLOON_SUCCESS));
	unlock_page(page);

	//if(newpage)
	//	set_bit(PG_nvram, &newpage->flags);

	//page->flags = 0;
	//printk(KERN_ALERT "__unmap_and_move: releasing page \n");
out:
	//dump_page(page);
	return rc;
}

/*
 * Obtain the lock on page, remove all ptes and migrate the page
 * to the newly allocated page in newpage.
 */
static int unmap_and_move(new_page_t get_new_page, unsigned long private,
			struct page *page, int force, enum migrate_mode mode)
{
	int rc = 0;
	int *result = NULL;
	struct page *newpage;
	int newhetmepage=0;


	newpage = get_new_page(page, private, &result);
	if (!newpage){
		nr_alloc_fail++;
		return -ENOMEM;
	}
	//return -ENOMEM;

	if(newpage->nvdirty == PAGE_MIGRATED){
		newhetmepage = 1;
	}

	if (page_count(page) == 1) {
		/* page was freed from under us. So we are done. */
		nr_page_freed++;
		goto out;
	}

	if (unlikely(PageTransHuge(page)))
		if (unlikely(split_huge_page(page)))
			goto out;

	rc = __unmap_and_move(page, newpage, force, mode);

	if (unlikely(rc == MIGRATEPAGE_BALLOON_SUCCESS)) {

		if(newhetmepage){
       		 printk(KERN_ALERT "In balloon page Setting page to migrated \n");
       		 newpage->nvdirty = PAGE_MIGRATED;
        	 hetero_add_to_nvlist(newpage);
    	}
		
		/*
		 * A ballooned page has been migrated already.
		 * Now, it's the time to wrap-up counters,
		 * handle the page back to Buddy and return.
		 */
		dec_zone_page_state(page, NR_ISOLATED_ANON +
				    page_is_file_cache(page));
		balloon_page_free(page);
		return MIGRATEPAGE_SUCCESS;
	}
out:
	if (rc != -EAGAIN) {

		if(newhetmepage){
       		 //printk(KERN_ALERT "unmap_move before (rc != -EAGAIN)\n");
       		 newpage->nvdirty = PAGE_MIGRATED;
        	 //hetero_add_to_nvlist(newpage);
    	}

		/*
		 * A page that has been migrated has all references
		 * removed and will be freed. A page that has not been
		 * migrated will have kepts its references and be
		 * restored.
		 */
		list_del(&page->lru);

		if (PageReserved(page))
			printk(KERN_ALERT "Releasing pages is reserved \n");

		dec_zone_page_state(page, NR_ISOLATED_ANON +
				page_is_file_cache(page));
		putback_lru_page(page);

		if(newhetmepage){
       		 //printk(KERN_ALERT "unmap_move before (rc != -EAGAIN)\n");
       		 newpage->nvdirty = PAGE_MIGRATED;
        	 hetero_add_to_nvlist(newpage);
    	}

	}

	//if (PageReserved(newpage))
	//	printk(KERN_ALERT "Releasing newpage is reserved \n");
	
	/*
	 * Move the new page to the LRU. If migration was not successful
	 * then this will free the page.
	 */
	//if (PageReserved(newpage))
	//	ClearPageReserved(newpage);
	//
	//
	if(newhetmepage){
		//printk(KERN_ALERT "Setting page to migrated \n");
		newpage->nvdirty = PAGE_MIGRATED;
		//hetero_add_to_nvlist(newpage);	
    }


	if(newhetmepage){
		//printk(KERN_ALERT "Adding pages to nvlist \n");
		newpage->nvdirty = PAGE_MIGRATED;
		hetero_add_to_nvlist(newpage);	
    }
	{
		putback_lru_page(newpage);
	}

	if (result) {
		if (rc)
			*result = rc;
		else
			*result = page_to_nid(newpage);
	}
	return rc;
}

/*
 * Counterpart of unmap_and_move_page() for hugepage migration.
 *
 * This function doesn't wait the completion of hugepage I/O
 * because there is no race between I/O and migration for hugepage.
 * Note that currently hugepage I/O occurs only in direct I/O
 * where no lock is held and PG_writeback is irrelevant,
 * and writeback status of all subpages are counted in the reference
 * count of the head page (i.e. if all subpages of a 2MB hugepage are
 * under direct I/O, the reference of the head page is 512 and a bit more.)
 * This means that when we try to migrate hugepage whose subpages are
 * doing direct I/O, some references remain after try_to_unmap() and
 * hugepage migration fails without data corruption.
 *
 * There is also no race when direct I/O is issued on the page under migration,
 * because then pte is replaced with migration swap entry and direct I/O code
 * will wait in the page fault for migration to complete.
 */
static int unmap_and_move_huge_page(new_page_t get_new_page,
				unsigned long private, struct page *hpage,
				int force, enum migrate_mode mode)
{
	int rc = 0;
	int *result = NULL;
	struct page *new_hpage = get_new_page(hpage, private, &result);
	struct anon_vma *anon_vma = NULL;

	if (!new_hpage)
		return -ENOMEM;

	rc = -EAGAIN;

	if (!trylock_page(hpage)) {
		if (!force || mode != MIGRATE_SYNC)
			goto out;
		lock_page(hpage);
	}

	if (PageAnon(hpage))
		anon_vma = page_get_anon_vma(hpage);

	try_to_unmap(hpage, TTU_MIGRATION|TTU_IGNORE_MLOCK|TTU_IGNORE_ACCESS);

	if (!page_mapped(hpage))
		rc = move_to_new_page(new_hpage, hpage, 1, mode);

	if (rc)
		remove_migration_ptes(hpage, hpage);

	if (anon_vma)
		put_anon_vma(anon_vma);

	if (!rc)
		hugetlb_cgroup_migrate(hpage, new_hpage);

	unlock_page(hpage);
out:
	put_page(new_hpage);
	if (result) {
		if (rc)
			*result = rc;
		else
			*result = page_to_nid(new_hpage);
	}
	return rc;
}

/*
 * migrate_pages
 *
 * The function takes one list of pages to migrate and a function
 * that determines from the page to be migrated and the private data
 * the target of the move and allocates the page.
 *
 * The function returns after 10 attempts or if no pages
 * are movable anymore because to has become empty
 * or no retryable pages exist anymore.
 * Caller should call putback_lru_pages to return pages to the LRU
 * or free list only if ret != 0.
 *
 * Return: Number of pages not migrated or error code.
 */
int migrate_pages(struct list_head *from, new_page_t get_new_page,
		unsigned long private, enum migrate_mode mode, int reason)
{
	int retry = 1;
	int nr_failed = 0;
	int nr_succeeded = 0;
	int pass = 0;
	struct page *page;
	struct page *page2;
	int swapwrite = current->flags & PF_SWAPWRITE;
	int rc;

	if (!swapwrite)
		current->flags |= PF_SWAPWRITE;

	for(pass = 0; pass < 10 && retry; pass++) {
		retry = 0;

		list_for_each_entry_safe(page, page2, from, lru) {
			cond_resched();

		  	/*if( (LRU_INACTIVE_ANON != page_lru_base_type(page))){
					//printk(KERN_ALERT "page LRU_ACTIVE_ANON %u\n",nr_failed);
	           		//printk(KERN_ALERT "page in LRU active list %u\n",
					//	nr_failed);
				rc = -1;
            }else if(!PageLRU(page)) {
					//printk(KERN_ALERT "page not LRU %u\n",nr_failed);
					rc = -1;
			}else */

			if(!page){
				printk( "migrate_pages: page for unmap_and_move is null\n");
				continue;
			}

			{
				rc = unmap_and_move(get_new_page, private,
						page, pass > 2, mode);
			}

			switch(rc) {
			case -ENOMEM:
				//inc_zone_page_state(page, NUMA_MIGRATE_FAILED);
				goto out;
			case -EAGAIN:
				retry++;
				//inc_zone_page_state(page, NUMA_MIGRATE_FAILED);
				break;
			case MIGRATEPAGE_SUCCESS:
/*NVM CHANGES*/
#ifdef HETERODEBUG
				 if (PageActive(page))
					printk("migrating active page\n");
				else 
					printk("migrating inactive page\n");
#endif
/*NVM CHANGES*/
				nr_succeeded++;
				inc_zone_page_state(page, NUMA_MIGRATED_FROM);
				break;

			default:
				/* Permanent failure */
				nr_failed++;
				//inc_zone_page_state(page, NUMA_MIGRATE_FAILED);
				break;
			}
		}
	}
	rc = nr_failed + retry;
out:
	if (nr_succeeded) {
		count_vm_events(PGMIGRATE_SUCCESS, nr_succeeded);
		//inc_zone_page_state(page, NUMA_MIGRATED_FROM);
	}
	if (nr_failed) {
		count_vm_events(PGMIGRATE_FAIL, nr_failed);
		//inc_zone_page_state(page, NUMA_MIGRATED_FROM);
		printk( "nr_failed %u \n", nr_failed);
	}

	trace_mm_migrate_pages(nr_succeeded, nr_failed, mode, reason);

	if (!swapwrite)
		current->flags &= ~PF_SWAPWRITE;

	return rc;
}


/*get the pages for hetero list*/
void init_hetero_list_fn(){

	unsigned int sz =0;
	if(!init_hetero_list){
		init_hetero_list =1;	
		INIT_LIST_HEAD(&hetero_list);
	}
	return;
}
EXPORT_SYMBOL(init_hetero_list);


/*get the pages for hetero list*/
struct list_head* get_hetero_list(unsigned int *nrpages){

	*nrpages = nr_heteropgcnt;

	return &hetero_list;
}
EXPORT_SYMBOL(get_hetero_list);




unsigned int debug_count_pagelist_cnt(struct list_head *from) {

    struct page *page;
    struct page *page2;
	unsigned int cnt =0;

	list_for_each_entry_safe(page, page2, from, lru) {
		cnt++;
	}
	return cnt;
}


/*
 * migrate_pages
 *
 * The function takes one list of pages to migrate and a function
 * that determines from the page to be migrated and the private data
 * the target of the move and allocates the page.
 *
 * The function returns after 10 attempts or if no pages
 * are movable anymore because to has become empty
 * or no retryable pages exist anymore.
 * Caller should call putback_lru_pages to return pages to the LRU
 * or free list only if ret != 0.
 *
 * Return: Number of pages not migrated or error code.
 */
int migrate_nearmem_pages(struct list_head *from, new_page_t get_new_page,
		unsigned long private, enum migrate_mode mode, int reason, int maxattempt)
{
	int retry = 1;
	int nr_failed = 0;
	int nr_succeeded = 0;
	int pass = 0;
	int nr_attempt;
	struct page *page;
	struct page *page2;
	int swapwrite = current->flags & PF_SWAPWRITE;
	int rc;

	if (!swapwrite)
		current->flags |= PF_SWAPWRITE;

	for(pass = 0; pass < 2 && retry; pass++) {
		retry = 0;

		list_for_each_entry_safe(page, page2, from, lru) {

			if( nr_attempt > maxattempt)
				goto out;
			else 
				nr_attempt++;

			cond_resched();

			if(!page){
				printk(KERN_ALERT "migrate_pages: page for unmap_and_move is null\n");
				continue;
			}
			nr_migrate_attempted++;	
			rc = unmap_and_move(get_new_page, private,
						page, pass > 2, mode);

			switch(rc) {
			case -ENOMEM:
				//inc_zone_page_state(page, NUMA_MIGRATE_FAILED);
				nr_migrate_failed++;
				break;
			case -EAGAIN:
				nr_migrate_retry++;
				retry++;
				//inc_zone_page_state(page, NUMA_MIGRATE_FAILED);
				break;
			case MIGRATEPAGE_SUCCESS:
				nr_succeeded++;
				nr_migrate_success++;
				nr_curr_migrate_success++;
				nr_reclaim_success++;
				inc_zone_page_state(page, NUMA_MIGRATED_FROM);
				if(page){
					//list_del(&page->nvlist);
					list_del(&page->lru);
					//__free_page(page);
					/*LRU list*/
					//page->nvpgoff = 0;
				}
				break;

			default:
				/* Permanent failure */
				nr_failed++;
				nr_migrate_failed++;
				//inc_zone_page_state(page, NUMA_MIGRATE_FAILED);
				break;
			}
		}
	}
	rc = nr_failed + retry;
out:
	if (nr_succeeded) {

		 printk(KERN_ALERT "migrate_nearmem_pages success %u\n", nr_succeeded);

		count_vm_events(PGMIGRATE_SUCCESS, nr_succeeded);
		//inc_zone_page_state(page, NUMA_MIGRATED_FROM);
	}
	if (nr_failed) {
		count_vm_events(PGMIGRATE_FAIL, nr_failed);
		//inc_zone_page_state(page, NUMA_MIGRATED_FROM);
		//printk( "nr_failed %u \n", nr_failed);
		 printk(KERN_ALERT "migrate_nearmem_pages failed %u\n", nr_succeeded);
	}
	trace_mm_migrate_pages(nr_succeeded, nr_failed, mode, reason);

	if (!swapwrite)
		current->flags &= ~PF_SWAPWRITE;

	return rc;
}



/*
 * migrate_pages
 *
 * The function takes one list of pages to migrate and a function
 * that determines from the page to be migrated and the private data
 * the target of the move and allocates the page.
 *
 * The function returns after 10 attempts or if no pages
 * are movable anymore because to has become empty
 * or no retryable pages exist anymore.
 * Caller should call putback_lru_pages to return pages to the LRU
 * or free list only if ret != 0.
 *
 * Return: Number of pages not migrated or error code.
 */
int my_migrate_pages(struct list_head *from, new_page_t get_new_page,
		unsigned long private, enum migrate_mode mode, int reason)
{
	int retry = 1;
	int nr_failed = 0;
	int nr_succeeded = 0;
	int pass = 0;
	struct page *page;
	struct page *page2;
	int swapwrite = current->flags & PF_SWAPWRITE;
	int rc;

	if (!swapwrite)
		current->flags |= PF_SWAPWRITE;

	for(pass = 0; pass < 1 && retry; pass++) {
		retry = 0;

		list_for_each_entry_safe(page, page2, from, lru) {

			cond_resched();

			if(!page){
				printk(KERN_ALERT "migrate_pages: page for unmap_and_move is null\n");
				continue;
			}

			nr_migrate_attempted++;	

			rc = unmap_and_move(get_new_page, private,
						page, pass > 2, mode);

			switch(rc) {
			case -ENOMEM:
				//inc_zone_page_state(page, NUMA_MIGRATE_FAILED);
				nr_migrate_failed++;
				break;
			case -EAGAIN:
				nr_migrate_retry++;
				retry++;
				//inc_zone_page_state(page, NUMA_MIGRATE_FAILED);
				break;
			case MIGRATEPAGE_SUCCESS:
				nr_succeeded++;
				nr_migrate_success++;
				nr_curr_migrate_success++;
				inc_zone_page_state(page, NUMA_MIGRATED_FROM);
				break;
			default:
				/* Permanent failure */
				nr_failed++;
				nr_migrate_failed++;
				//inc_zone_page_state(page, NUMA_MIGRATE_FAILED);
				break;
			}
		}
	}
	rc = nr_failed + retry;
out:
	if (nr_succeeded) {
		count_vm_events(PGMIGRATE_SUCCESS, nr_succeeded);
		//printk( "nr_succeeded migration %u \n", nr_succeeded);
		//inc_zone_page_state(page, NUMA_MIGRATED_FROM);
	}
	if (nr_failed) {
		count_vm_events(PGMIGRATE_FAIL, nr_failed);
		//inc_zone_page_state(page, NUMA_MIGRATED_FROM);
		printk( "nr_failed %u \n", nr_failed);
	}
	trace_mm_migrate_pages(nr_succeeded, nr_failed, mode, reason);

	if (!swapwrite)
		current->flags &= ~PF_SWAPWRITE;

	return rc;
}

int migrate_huge_page(struct page *hpage, new_page_t get_new_page,
		      unsigned long private, enum migrate_mode mode)
{
	int pass, rc;

	for (pass = 0; pass < 10; pass++) {
		rc = unmap_and_move_huge_page(get_new_page, private,
						hpage, pass > 2, mode);
		switch (rc) {
		case -ENOMEM:
			goto out;
		case -EAGAIN:
			/* try again */
			cond_resched();
			break;
		case MIGRATEPAGE_SUCCESS:
			goto out;
		default:
			rc = -EIO;
			goto out;
		}
	}
out:
	return rc;
}

#ifdef CONFIG_NUMA
/*
 * Move a list of individual pages
 */
struct page_to_node {
	unsigned long addr;
	struct page *page;
	int node;
	int status;
};

static struct page *new_slowpage(struct page *p, unsigned long private,
		int **result)
{
	struct page *page = NULL;
	struct page_to_node *pm = (struct page_to_node *)private;

	if(!pm) {	
		printk(KERN_ALERT "new_page_node: page to node failed \n");
		return NULL;			
	}

	if(!p) {
		printk(KERN_ALERT "new_page_node: source page *p is null \n");
		return NULL;
	}	

   page = alloc_page(GFP_HIGHUSER);
   //page = alloc_pages_exact_node(private,
	//	                   GFP_HIGHUSER_MOVABLE | GFP_THISNODE, 0);
	if(!page){
		printk(KERN_ALERT "new_slowpage alloc failed \n");
        return NULL;
	}
	if (page && page->nvdirty == PAGE_MIGRATED) {
		printk(KERN_ALERT "new_slowpage alloc failed \n");
		return NULL;
	}

	if(page) {
		printk(KERN_ALERT "page allocation successful \n");
	}

	return page;
}


static struct page *new_page_node(struct page *p, unsigned long private,
		int **result)
{
	struct page *page = NULL;
	struct page_to_node *pm = (struct page_to_node *)private;

	if(!pm) {	
		printk(KERN_ALERT "new_page_node: page to node failed \n");
		return NULL;			
	}

	if(!p) {
		printk(KERN_ALERT "new_page_node: source page *p is null \n");
		return NULL;
	}	

#ifdef ENABLE_HETERO
#if 0
	page = alloc_page(GFP_HIGHUSER);
	if(page){
        //ClearPageReserved(page);
		//init_page_count(page);
	}
#else
	//page = hetero_getnxt_page(false);
	page = hetero_alloc_migrate(GFP_HIGHUSER_MOVABLE |__GFP_ZERO, 0, 0);
	if(!page){
		//printk(KERN_ALERT "new_page_node: getting heteropage FAILED \n");
        return NULL;
	}else{
		//page->flags = page->flags | PG_nvram;
		//set_bit(PG_hetero, &page->flags);
		hetero_add_to_nvlist(page);
	} 
#endif
	return page;
  
#else
	return alloc_pages_exact_node(private,
				GFP_HIGHUSER_MOVABLE | GFP_THISNODE, 0);
#endif
}

/*
 * Move a set of pages as indicated in the pm array. The addr
 * field must be set to the virtual address of the page to be moved
 * and the node number must contain a valid target node.
 * The pm array ends with node = MAX_NUMNODES.
 */
static int do_move_page_to_node_array(struct mm_struct *mm,
				      struct page_to_node *pm,
				      int migrate_all)
{
	int err;
	struct page_to_node *pp;
	LIST_HEAD(pagelist);

	down_read(&mm->mmap_sem);

	/*
	 * Build a list of pages to migrate
	 */
	for (pp = pm; pp->node != MAX_NUMNODES; pp++) {
		struct vm_area_struct *vma;
		struct page *page;

		err = -EFAULT;
		vma = find_vma(mm, pp->addr);
		if (!vma || pp->addr < vma->vm_start || !vma_migratable(vma))
			goto set_status;

		page = follow_page(vma, pp->addr, FOLL_GET|FOLL_SPLIT);

		err = PTR_ERR(page);
		if (IS_ERR(page)) {
			pg_nopte++;
			goto set_status;
		}

		err = -ENOENT;
		if (!page) {
			pg_notexist++;	
			goto set_status;
		}

		/* Use PageReserved to check for zero page */
		if (PageReserved(page))
			goto put_and_set;

		pp->page = page;
		err = page_to_nid(page);

		if (err == pp->node) {

			 pg_rtnode_err++;
			/*
			 * Node already in the right place
			 */
			goto put_and_set;
		}

		err = -EACCES;
		if (page_mapcount(page) > 1 &&
				!migrate_all)
			goto put_and_set;


		if(PageLRU(page)) {
			//printk(KERN_ALERT "do_move_page_to_node_array page is LRU \n");
		}else {
			//printk(KERN_ALERT "do_move_page_to_node_array not LRU \n");
		}
		err = isolate_lru_page(page);
		if (!err) {
			list_add_tail(&page->lru, &pagelist);
			inc_zone_page_state(page, NR_ISOLATED_ANON +
					    page_is_file_cache(page));
		}
		if(err == -EBUSY) {
			pg_busy++;
		}
put_and_set:
		/*
		 * Either remove the duplicate refcount from
		 * isolate_lru_page() or drop the page ref if it was
		 * not isolated.
		 */
		put_page(page);
set_status:
		pp->status = err;
	}

	err = 0;
	if (!list_empty(&pagelist)) {
		err = migrate_pages(&pagelist, new_page_node,
				(unsigned long)pm, MIGRATE_SYNC, MR_SYSCALL);
		if (err)
			putback_lru_pages(&pagelist);
	}

	up_read(&mm->mmap_sem);
	return err;
}

/*
 * Migrate an array of page address onto an array of nodes and fill
 * the corresponding array of status.
 */
static int do_pages_move(struct mm_struct *mm, nodemask_t task_nodes,
			 unsigned long nr_pages,
			 const void __user * __user *pages,
			 const int __user *nodes,
			 int __user *status, int flags)
{
	struct page_to_node *pm;
	unsigned long chunk_nr_pages;
	unsigned long chunk_start;
	int err;

	pg_busy=0;
	pg_rtnode_err=0;
	pg_notexist=0;
	pg_nopte=0;


	err = -ENOMEM;
	pm = (struct page_to_node *)__get_free_page(GFP_KERNEL);
	if (!pm)
		goto out;

	migrate_prep();

	/*
	 * Store a chunk of page_to_node array in a page,
	 * but keep the last one as a marker
	 */
	chunk_nr_pages = (PAGE_SIZE / sizeof(struct page_to_node)) - 1;

	for (chunk_start = 0;
	     chunk_start < nr_pages;
	     chunk_start += chunk_nr_pages) {
		int j;

		if (chunk_start + chunk_nr_pages > nr_pages)
			chunk_nr_pages = nr_pages - chunk_start;

		/* fill the chunk pm with addrs and nodes from user-space */
		for (j = 0; j < chunk_nr_pages; j++) {
			const void __user *p;
			int node;

			err = -EFAULT;
			if (get_user(p, pages + j + chunk_start))
				goto out_pm;
			pm[j].addr = (unsigned long) p;

			if (get_user(node, nodes + j + chunk_start))
				goto out_pm;

			err = -ENODEV;
			if (node < 0 || node >= MAX_NUMNODES)
				goto out_pm;

			if (!node_state(node, N_MEMORY))
				goto out_pm;

			err = -EACCES;
			if (!node_isset(node, task_nodes))
				goto out_pm;

			pm[j].node = node;
		}

		/* End marker for this chunk */
		pm[chunk_nr_pages].node = MAX_NUMNODES;

		/* Migrate this chunk */
		err = do_move_page_to_node_array(mm, pm,
						 flags & MPOL_MF_MOVE_ALL);
		if (err < 0)
			goto out_pm;

		/* Return status information */
		for (j = 0; j < chunk_nr_pages; j++)
			if (put_user(pm[j].status, status + j + chunk_start)) {
				err = -EFAULT;
				goto out_pm;
			}
	}
	err = 0;

out_pm:
	free_page((unsigned long)pm);
out:
	return err;
}

/*
 * Determine the nodes of an array of pages and store it in an array of status.
 */
static void do_pages_stat_array(struct mm_struct *mm, unsigned long nr_pages,
				const void __user **pages, int *status)
{
	unsigned long i;

	down_read(&mm->mmap_sem);

	for (i = 0; i < nr_pages; i++) {
		unsigned long addr = (unsigned long)(*pages);
		struct vm_area_struct *vma;
		struct page *page;
		int err = -EFAULT;

		vma = find_vma(mm, addr);
		if (!vma || addr < vma->vm_start)
			goto set_status;

		page = follow_page(vma, addr, 0);

		err = PTR_ERR(page);
		if (IS_ERR(page))
			goto set_status;

		err = -ENOENT;
		/* Use PageReserved to check for zero page */
		if (!page || PageReserved(page))
			goto set_status;

		err = page_to_nid(page);
set_status:
		*status = err;

		pages++;
		status++;
	}

	up_read(&mm->mmap_sem);
}

/*
 * Determine the nodes of a user array of pages and store it in
 * a user array of status.
 */
static int do_pages_stat(struct mm_struct *mm, unsigned long nr_pages,
			 const void __user * __user *pages,
			 int __user *status)
{
#define DO_PAGES_STAT_CHUNK_NR 16
	const void __user *chunk_pages[DO_PAGES_STAT_CHUNK_NR];
	int chunk_status[DO_PAGES_STAT_CHUNK_NR];

	while (nr_pages) {
		unsigned long chunk_nr;

		chunk_nr = nr_pages;
		if (chunk_nr > DO_PAGES_STAT_CHUNK_NR)
			chunk_nr = DO_PAGES_STAT_CHUNK_NR;

		if (copy_from_user(chunk_pages, pages, chunk_nr * sizeof(*chunk_pages)))
			break;

		do_pages_stat_array(mm, chunk_nr, chunk_pages, chunk_status);

		if (copy_to_user(status, chunk_status, chunk_nr * sizeof(*status)))
			break;

		pages += chunk_nr;
		status += chunk_nr;
		nr_pages -= chunk_nr;
	}
	return nr_pages ? -EFAULT : 0;
}

/*
 * Move a list of pages in the address space of the currently executing
 * process.
 */
SYSCALL_DEFINE6(move_pages, pid_t, pid, unsigned long, nr_pages,
		const void __user * __user *, pages,
		const int __user *, nodes,
		int __user *, status, int, flags)
{
	const struct cred *cred = current_cred(), *tcred;
	struct task_struct *task;
	struct mm_struct *mm;
	int err;
	nodemask_t task_nodes;

	/* Check flags */
	if (flags & ~(MPOL_MF_MOVE|MPOL_MF_MOVE_ALL))
		return -EINVAL;

	if ((flags & MPOL_MF_MOVE_ALL) && !capable(CAP_SYS_NICE))
		return -EPERM;

	/* Find the mm_struct */
	rcu_read_lock();
	task = pid ? find_task_by_vpid(pid) : current;
	if (!task) {
		rcu_read_unlock();
		return -ESRCH;
	}
	get_task_struct(task);

	/*
	 * Check if this process has the right to modify the specified
	 * process. The right exists if the process has administrative
	 * capabilities, superuser privileges or the same
	 * userid as the target process.
	 */
	tcred = __task_cred(task);
	if (!uid_eq(cred->euid, tcred->suid) && !uid_eq(cred->euid, tcred->uid) &&
	    !uid_eq(cred->uid,  tcred->suid) && !uid_eq(cred->uid,  tcred->uid) &&
	    !capable(CAP_SYS_NICE)) {
		rcu_read_unlock();
		err = -EPERM;
		goto out;
	}
	rcu_read_unlock();

 	err = security_task_movememory(task);
 	if (err)
		goto out;

	task_nodes = cpuset_mems_allowed(task);
	mm = get_task_mm(task);
	put_task_struct(task);

	if (!mm)
		return -EINVAL;

	if (nodes)
		err = do_pages_move(mm, task_nodes, nr_pages, pages,
				    nodes, status, flags);
	else
		err = do_pages_stat(mm, nr_pages, pages, status);

	mmput(mm);

	printk( "move_pages succeess: %lu out of %lu "
					  "pg_busy %u, pg_rtnode_err %u, "
					  "pg_notexist %u, pg_nopte %u \n",
					   nr_migrate_success,nr_pages,
					   pg_busy, pg_rtnode_err, pg_notexist, pg_nopte);
	return err;

out:

	printk( "move_pages succeess: %lu out of %lu\n",
			 nr_migrate_success,nr_pages);

	put_task_struct(task);
	return err;
}

/*
 * Call migration functions in the vma_ops that may prepare
 * memory in a vm for migration. migration functions may perform
 * the migration for vmas that do not have an underlying page struct.
 */
int migrate_vmas(struct mm_struct *mm, const nodemask_t *to,
	const nodemask_t *from, unsigned long flags)
{
 	struct vm_area_struct *vma;
 	int err = 0;

	for (vma = mm->mmap; vma && !err; vma = vma->vm_next) {
 		if (vma->vm_ops && vma->vm_ops->migrate) {
 			err = vma->vm_ops->migrate(vma, to, from, flags);
 			if (err)
 				break;
 		}
 	}
 	return err;
}

#ifdef CONFIG_NUMA_BALANCING
/*
 * Returns true if this is a safe migration target node for misplaced NUMA
 * pages. Currently it only checks the watermarks which crude
 */
static bool migrate_balanced_pgdat(struct pglist_data *pgdat,
				   unsigned long nr_migrate_pages)
{
	int z;
	for (z = pgdat->nr_zones - 1; z >= 0; z--) {
		struct zone *zone = pgdat->node_zones + z;

		if (!populated_zone(zone))
			continue;

		if (zone->all_unreclaimable)
			continue;

		/* Avoid waking kswapd by allocating pages_to_migrate pages. */
		if (!zone_watermark_ok(zone, 0,
				       high_wmark_pages(zone) +
				       nr_migrate_pages,
				       0, 0))
			continue;
		return true;
	}
	return false;
}

static struct page *alloc_misplaced_dst_page(struct page *page,
					   unsigned long data,
					   int **result)
{
	int nid = (int) data;
	struct page *newpage;

	newpage = alloc_pages_exact_node(nid,
					 (GFP_HIGHUSER_MOVABLE | GFP_THISNODE |
					  __GFP_NOMEMALLOC | __GFP_NORETRY |
					  __GFP_NOWARN) &
					 ~GFP_IOFS, 0);
	if (newpage)
		page_nid_xchg_last(newpage, page_nid_last(page));

	return newpage;
}

/*
 * page migration rate limiting control.
 * Do not migrate more than @pages_to_migrate in a @migrate_interval_millisecs
 * window of time. Default here says do not migrate more than 1280M per second.
 * If a node is rate-limited then PTE NUMA updates are also rate-limited. However
 * as it is faults that reset the window, pte updates will happen unconditionally
 * if there has not been a fault since @pteupdate_interval_millisecs after the
 * throttle window closed.
 */
static unsigned int migrate_interval_millisecs __read_mostly = 100;
static unsigned int pteupdate_interval_millisecs __read_mostly = 1000;
static unsigned int ratelimit_pages __read_mostly = 128 << (20 - PAGE_SHIFT);

/* Returns true if NUMA migration is currently rate limited */
bool migrate_ratelimited(int node)
{
	pg_data_t *pgdat = NODE_DATA(node);

	if (time_after(jiffies, pgdat->numabalancing_migrate_next_window +
				msecs_to_jiffies(pteupdate_interval_millisecs)))
		return false;

	if (pgdat->numabalancing_migrate_nr_pages < ratelimit_pages)
		return false;

	return true;
}

/* Returns true if the node is migrate rate-limited after the update */
bool numamigrate_update_ratelimit(pg_data_t *pgdat, unsigned long nr_pages)
{
	bool rate_limited = false;

	/*
	 * Rate-limit the amount of data that is being migrated to a node.
	 * Optimal placement is no good if the memory bus is saturated and
	 * all the time is being spent migrating!
	 */
	spin_lock(&pgdat->numabalancing_migrate_lock);
	if (time_after(jiffies, pgdat->numabalancing_migrate_next_window)) {
		pgdat->numabalancing_migrate_nr_pages = 0;
		pgdat->numabalancing_migrate_next_window = jiffies +
			msecs_to_jiffies(migrate_interval_millisecs);
	}
	if (pgdat->numabalancing_migrate_nr_pages > ratelimit_pages)
		rate_limited = true;
	else
		pgdat->numabalancing_migrate_nr_pages += nr_pages;
	spin_unlock(&pgdat->numabalancing_migrate_lock);
	
	return rate_limited;
}

int numamigrate_isolate_page(pg_data_t *pgdat, struct page *page)
{
	int page_lru;

	VM_BUG_ON(compound_order(page) && !PageTransHuge(page));

	//printk( "numamigrate_isolate_page calling isolate lru \n");

	/* Avoid migrating to a node that is nearly full */
	if (!migrate_balanced_pgdat(pgdat, 1UL << compound_order(page)))
		return 0;

	if (isolate_lru_page(page))
		return 0;

	/*
	 * migrate_misplaced_transhuge_page() skips page migration's usual
	 * check on page_count(), so we must do it here, now that the page
	 * has been isolated: a GUP pin, or any other pin, prevents migration.
	 * The expected page count is 3: 1 for page's mapcount and 1 for the
	 * caller's pin and 1 for the reference taken by isolate_lru_page().
	 */
	if (PageTransHuge(page) && page_count(page) != 3) {
		putback_lru_page(page);
		return 0;
	}

	page_lru = page_is_file_cache(page);
	mod_zone_page_state(page_zone(page), NR_ISOLATED_ANON + page_lru,
				hpage_nr_pages(page));

	/*
	 * Isolating the page has taken another reference, so the
	 * caller's reference can be safely dropped without the page
	 * disappearing underneath us during migration.
	 */
	put_page(page);
	return 1;
}

/*
 * Attempt to migrate a misplaced page to the specified destination
 * node. Caller is expected to have an elevated reference count on
 * the page that will be dropped by this function before returning.
 */
int migrate_misplaced_page(struct page *page, int node)
{
	pg_data_t *pgdat = NODE_DATA(node);
	int isolated;
	int nr_remaining;
	LIST_HEAD(migratepages);

	/*
	 * Don't migrate pages that are mapped in multiple processes.
	 * TODO: Handle false sharing detection instead of this hammer
	 */
	if (page_mapcount(page) != 1)
		goto out;

	/*
	 * Rate-limit the amount of data that is being migrated to a node.
	 * Optimal placement is no good if the memory bus is saturated and
	 * all the time is being spent migrating!
	 */
	if (numamigrate_update_ratelimit(pgdat, 1))
		goto out;

	isolated = numamigrate_isolate_page(pgdat, page);
	if (!isolated)
		goto out;

	list_add(&page->lru, &migratepages);
	nr_remaining = migrate_pages(&migratepages, alloc_misplaced_dst_page,
				     node, MIGRATE_ASYNC, MR_NUMA_MISPLACED);
	if (nr_remaining) {
		putback_lru_pages(&migratepages);
		isolated = 0;
	} else
		count_vm_numa_event(NUMA_PAGE_MIGRATE);
	BUG_ON(!list_empty(&migratepages));
	return isolated;

out:
	put_page(page);
	return 0;
}
#endif /* CONFIG_NUMA_BALANCING */

#if defined(CONFIG_NUMA_BALANCING) && defined(CONFIG_TRANSPARENT_HUGEPAGE)
/*
 * Migrates a THP to a given target node. page must be locked and is unlocked
 * before returning.
 */
int migrate_misplaced_transhuge_page(struct mm_struct *mm,
				struct vm_area_struct *vma,
				pmd_t *pmd, pmd_t entry,
				unsigned long address,
				struct page *page, int node)
{
	unsigned long haddr = address & HPAGE_PMD_MASK;
	pg_data_t *pgdat = NODE_DATA(node);
	int isolated = 0;
	struct page *new_page = NULL;
	struct mem_cgroup *memcg = NULL;
	int page_lru = page_is_file_cache(page);

	/*
	 * Don't migrate pages that are mapped in multiple processes.
	 * TODO: Handle false sharing detection instead of this hammer
	 */
	if (page_mapcount(page) != 1)
		goto out_dropref;

	/*
	 * Rate-limit the amount of data that is being migrated to a node.
	 * Optimal placement is no good if the memory bus is saturated and
	 * all the time is being spent migrating!
	 */
	if (numamigrate_update_ratelimit(pgdat, HPAGE_PMD_NR))
		goto out_dropref;

	new_page = alloc_pages_node(node,
		(GFP_TRANSHUGE | GFP_THISNODE) & ~__GFP_WAIT, HPAGE_PMD_ORDER);
	if (!new_page)
		goto out_fail;

	page_nid_xchg_last(new_page, page_nid_last(page));

	isolated = numamigrate_isolate_page(pgdat, page);
	if (!isolated) {
		put_page(new_page);
		goto out_fail;
	}

	/* Prepare a page as a migration target */
	__set_page_locked(new_page);
	SetPageSwapBacked(new_page);

	/* anon mapping, we can simply copy page->mapping to the new page: */
	new_page->mapping = page->mapping;
	new_page->index = page->index;
	migrate_page_copy(new_page, page);
	WARN_ON(PageLRU(new_page));

	/* Recheck the target PMD */
	spin_lock(&mm->page_table_lock);
	if (unlikely(!pmd_same(*pmd, entry))) {
		spin_unlock(&mm->page_table_lock);

		/* Reverse changes made by migrate_page_copy() */
		if (TestClearPageActive(new_page))
			SetPageActive(page);
		if (TestClearPageUnevictable(new_page))
			SetPageUnevictable(page);
		mlock_migrate_page(page, new_page);

		unlock_page(new_page);
		put_page(new_page);		/* Free it */

		unlock_page(page);
		putback_lru_page(page);

		count_vm_events(PGMIGRATE_FAIL, HPAGE_PMD_NR);
		isolated = 0;
		goto out;
	}

	/*
	 * Traditional migration needs to prepare the memcg charge
	 * transaction early to prevent the old page from being
	 * uncharged when installing migration entries.  Here we can
	 * save the potential rollback and start the charge transfer
	 * only when migration is already known to end successfully.
	 */
	mem_cgroup_prepare_migration(page, new_page, &memcg);

	entry = mk_pmd(new_page, vma->vm_page_prot);
	entry = pmd_mknonnuma(entry);
	entry = maybe_pmd_mkwrite(pmd_mkdirty(entry), vma);
	entry = pmd_mkhuge(entry);

	page_add_new_anon_rmap(new_page, vma, haddr);

	set_pmd_at(mm, haddr, pmd, entry);
	update_mmu_cache_pmd(vma, address, &entry);
	page_remove_rmap(page);
	/*
	 * Finish the charge transaction under the page table lock to
	 * prevent split_huge_page() from dividing up the charge
	 * before it's fully transferred to the new page.
	 */
	mem_cgroup_end_migration(memcg, page, new_page, true);
	spin_unlock(&mm->page_table_lock);

	unlock_page(new_page);
	unlock_page(page);
	put_page(page);			/* Drop the rmap reference */
	put_page(page);			/* Drop the LRU isolation reference */

	count_vm_events(PGMIGRATE_SUCCESS, HPAGE_PMD_NR);
	count_vm_numa_events(NUMA_PAGE_MIGRATE, HPAGE_PMD_NR);

out:
	mod_zone_page_state(page_zone(page),
			NR_ISOLATED_ANON + page_lru,
			-HPAGE_PMD_NR);
	return isolated;

out_fail:
	count_vm_events(PGMIGRATE_FAIL, HPAGE_PMD_NR);
out_dropref:
	unlock_page(page);
	put_page(page);
	return 0;
}


//NVM CHNAGES SUDARSUN

struct migrate_maps {
    struct vm_area_struct *vma;
    unsigned long pages;
    unsigned long anon;
    unsigned long active;
    unsigned long writeback;
    unsigned long mapcount_max;
    unsigned long dirty;
    unsigned long swapcache;
    unsigned long node[MAX_NUMNODES];
};

unsigned int inactpgcnt=0;




#if 0
/*
 * Migrate an array of page address onto an array of nodes and fill
 * the corresponding array of status.
 */
static int hetero_do_pages_move(struct mm_struct *mm, unsigned long nr_pages,
			 int flags, struct list_head *migratepages)
{
	struct page_to_node *pm;
	unsigned long chunk_nr_pages;
	unsigned long chunk_start;
	int err;
	struct page *pg =NULL;
	struct list_head *tmp;
	int migrate_all=1;
	int pagecnt=0;
	int node_to_migrate=1;

	err = -ENOMEM;

	migrate_prep();

	struct page_to_node *pp;
	struct page *page, *page2;
	unsigned int pagelstcnt=0, isolate_cnt=0;

	printk( "migratepages count %u \n", inactpgcnt);

	if(list_empty(&mylist)) {
		printk( "migratepages list empty \n");
		return 0;	
	}

	LIST_HEAD(pagelist);

	down_read(&mm->mmap_sem);

	list_for_each_entry_safe(page, page2, &mylist, lru) {

		err = PTR_ERR(page);
		if (IS_ERR(page))
			goto set_status;
	
		if (!page)
			goto set_status;

		/* Use PageReserved to check for zero page */
		if (PageReserved(page))
			goto put_and_set;

		err = page_to_nid(page);

		err = -EACCES;
		if (page_mapcount(page) > 1 &&
				!migrate_all)
			goto put_and_set;

		err = isolate_lru_page(page);
		if (!err) {

			printk( "isolating page %u\n",isolate_cnt);	
  		    list_del(&page->nvlist);
			inc_zone_page_state(page, NR_ISOLATED_ANON +
					    page_is_file_cache(page));
		}
put_and_set:
		/*
		 * Either remove the duplicate refcount from
		 * isolate_lru_page() or drop the page ref if it was
		 * not isolated.
		 */
		put_page(page);

		pagelstcnt++;
	}
	printk( "page count %u \n", pagelstcnt);

	err = 0;
	if (!list_empty(&pagelist)) {
		err = migrate_pages(&pagelist, new_page_node,
			 (unsigned long)node_to_migrate, MIGRATE_SYNC, MR_SYSCALL);
			 printk( "migrate_pages: nr_migrate_success %u\n",nr_migrate_success);

	if (err)
		putback_lru_pages(&pagelist);

	}else {
		 printk(KERN_ALERT "migrate_pages: pagelist is empty \n");
	}

	
set_status:
		if(err)
			printk(KERN_ALERT "finishing migration do_move_page with error\n");
		else 
			printk(KERN_ALERT "migrate_pages: finished without errors %u\n", nr_migrate_success);
		
		up_read(&mm->mmap_sem);

		return err;

}
#endif

#if 0
int hetero_migrate_isolate_page(pg_data_t *pgdat, struct page *page)
{
	int page_lru;

	VM_BUG_ON(compound_order(page) && !PageTransHuge(page));

	/* Avoid migrating to a node that is nearly full */
	if (!migrate_balanced_pgdat(pgdat, 1UL << compound_order(page))) {
		printk(KERN_ALERT "failed isolation, full node \n");		
		return 0;
	}
	
	if(!PageLRU(page)){
		printk(KERN_ALERT "not lru page \n");		
		return 0;
	}else {
#ifdef HETERODEBUG
		printk(KERN_ALERT "page is in LRU\n");		
#endif
		if(is_active_lru(page_lru_base_type(page))){
			printk(KERN_ALERT "page is in LRU active list\n");		
			return 0; 
		}else {
#ifdef HETERODEBUG
			printk(KERN_ALERT "page is in LRU inactive list\n");		
#endif
		}
	}


	if (isolate_lru_page(page)) {
		printk(KERN_ALERT "failed isolation, lru page \n");		
		return 0;
	}

	/*
	 * migrate_misplaced_transhuge_page() skips page migration's usual
	 * check on page_count(), so we must do it here, now that the page
	 * has been isolated: a GUP pin, or any other pin, prevents migration.
	 * The expected page count is 3: 1 for page's mapcount and 1 for the
	 * caller's pin and 1 for the reference taken by isolate_lru_page().
	 */
	if (PageTransHuge(page) && page_count(page) != 3) {
		putback_lru_page(page);
		return 0;
	}

	page_lru = page_is_file_cache(page);
	mod_zone_page_state(page_zone(page), NR_ISOLATED_ANON + page_lru,
				hpage_nr_pages(page));

	/*
	 * Isolating the page has taken another reference, so the
	 * caller's reference can be safely dropped without the page
	 * disappearing underneath us during migration.
	 */
	put_page(page);
	return 1;
}



/*
 * Attempt to migrate a misplaced page to the specified destination
 * node. Caller is expected to have an elevated reference count on
 * the page that will be dropped by this function before returning.
 */
int hetero_migrate_page(struct page *page, int node)
{
	pg_data_t *pgdat = NODE_DATA(node);
	int isolated;
	int nr_remaining;
	LIST_HEAD(migratepages);

	/*
	 * Don't migrate pages that are mapped in multiple processes.
	 * TODO: Handle false sharing detection instead of this hammer
	 */
	if (page_mapcount(page) != 1) {
		printk( "page map count %d \n",page_mapcount(page));
		goto out;
	}
	
#if 0
	/*
	 * Rate-limit the amount of data that is being migrated to a node.
	 * Optimal placement is no good if the memory bus is saturated and
	 * all the time is being spent migrating!
	 */
	/*if (numamigrate_update_ratelimit(pgdat, 1)) {
		printk( "ratelimit exceeded \n");
		goto out;
	}*/	
#endif

	isolated = hetero_migrate_isolate_page(pgdat, page);
	if (!isolated) {
		printk(KERN_ALERT "page could not be isolated\n");
		goto out;
	}else {
#ifdef HETERODEBUG
		printk(KERN_ALERT "page is isolated\n");
#endif
		//goto out;
	}

	list_add(&page->lru, &migratepages);
	nr_remaining = migrate_pages(&migratepages, alloc_misplaced_dst_page,
				     node, MIGRATE_SYNC, MR_NUMA_MISPLACED);
	if (nr_remaining) {
		putback_lru_pages(&migratepages);
		isolated = 0;
	} 
	else
		count_vm_numa_event(NUMA_PAGE_MIGRATE);

	if (nr_remaining) {
		printk(KERN_ALERT "migration failed for %d\n", nr_remaining);
		goto out;
	}

	BUG_ON(!list_empty(&migratepages));

	return isolated;

out:
	put_page(page);
	return 0;
}

static struct page *mig_can_gather_numa_stats(pte_t pte, struct vm_area_struct *vma,
        unsigned long addr)
{
    struct page *page;
    int nid, ret = -1;

    if (!pte_present(pte))
        return NULL;

    page = vm_normal_page(vma, addr, pte);
    if (!page)
        return NULL;

    if (PageReserved(page)){
		printk("Page Reserved\n");
        return NULL;
	}

    nid = page_to_nid(page);
    /*if (!node_isset(nid, node_states[N_MEMORY]))
        return NULL;*/
	//if (PageActive(page)) 
	{
#ifdef HETERODEBUG
		printk("Page active, nodeid %d "
			   "inactpgcnt %u \n", nid, inactpgcnt);
#endif
		if(nid ==0) {
			//nid = 1;
			//ret = hetero_migrate_page(page, nid);
			inactpgcnt++;
			list_add(&page->nvlist, &mylist);
		}
#if 0		
		if(!ret) {
#ifdef HETERODEBUG
			printk("Migration successfull \n");
#endif
		}else {
			printk("Migration failed \n");
		}
#endif

	}/*else{
		//printk("Page inactive \n");
	}*/
    return page;
}

static int mig_gather_pte_stats(pmd_t *pmd, unsigned long addr,
        unsigned long end, struct mm_walk *walk)
{
    struct migrate_maps *md;
    spinlock_t *ptl;
    pte_t *orig_pte;
    pte_t *pte;

    md = walk->private;

    if (pmd_trans_unstable(pmd))
        return 0;
    orig_pte = pte = pte_offset_map_lock(walk->mm, pmd, addr, &ptl);
    do {
        struct page *page = mig_can_gather_numa_stats(*pte, md->vma, addr);
        if (!page)
            continue;

    } while (pte++, addr += PAGE_SIZE, addr != end);
    pte_unmap_unlock(orig_pte, ptl);
    return 0;
}


static void check_page_stat(int node, int zid, enum lru_list lru)
{
    struct lruvec *lruvec;
    unsigned long flags;
    struct list_head *list;
    struct page *busy;
    struct zone *zone;
	struct mem_cgroup *memcg;
	unsigned int pgcnt=0;

   	memcg = mem_cgroup_iter(NULL, NULL, NULL);
    zone = &NODE_DATA(node)->node_zones[zid];
    lruvec = mem_cgroup_zone_lruvec(zone, memcg);

	if(!lruvec){
		printk(KERN_ALERT "lruvec is null \n");
		return;
	}

	printk("going to iterate lru for zid %d, node %d\n",zid, node);

    list = &lruvec->lists[lru];
    busy = NULL;
    do {

        //struct page_cgroup *pc;
        struct page *page;
        //spin_lock_irqsave(&zone->lru_lock, flags);
		//printk(KERN_ALERT "ACQUIRED LOCK \n");

        if (list_empty(list)) {
			printk(KERN_ALERT "lruvec list is null for zid %d, node %d\n",zid, node);
			printk(KERN_ALERT "RELEASED LOCK empty list\n");
            //spin_unlock_irqrestore(&zone->lru_lock, flags);
            break;
        }
        page = list_entry(list->prev, struct page, lru);
        if (busy == page) {
            //list_move(&page->lru, list);
            busy = NULL;
			printk(KERN_ALERT "RELEASED LOCK busy == page \n");
            //spin_unlock_irqrestore(&zone->lru_lock, flags);
            continue;
        }

		if(page) {
		  if (!PageActive(page)) {
        		inactpgcnt++;
        		printk("Page Inactive, nodeid %d "
              	"inactpgcnt %u \n", node, inactpgcnt);
			}
		}
		//printk(KERN_ALERT "RELEASED LOCK After ActivePg Check \n");
        //spin_unlock_irqrestore(&zone->lru_lock, flags);
       #if 0 
       pc = lookup_page_cgroup(page);
        if (mem_cgroup_move_parent(page, pc, memcg)) {
            /* found lock contention or "pc" is obsolete. */
            busy = page;
            cond_resched();
         } else
            busy = NULL;
     #endif	

		pgcnt++;
		
		if(pgcnt > 100) return 0;

    } while (!list_empty(list));
}

void iterate_all_lrulist() {

 int node, zid; 
  	
  for_each_node_state(node, N_MEMORY) {
     for (zid = 1; zid < MAX_NR_ZONES; zid++) {
         enum lru_list lru;
         for_each_lru(lru) {
			printk("Checking LRU \n");
   			check_page_stat(node, zid, lru);
			printk("Finished Checking LRU \n");
			return;
		 }
      }
   }
}
#endif

/*
 * page migration
 */
static void migrate_page_add(struct page *page, struct list_head *pagelist,
				unsigned long flags)
{
	/*
	 * Avoid migrating a page that is shared with others.
	 */
	if ((flags & MPOL_MF_MOVE_ALL) || page_mapcount(page) == 1) {
		if (!isolate_lru_page(page)) {
			list_add_tail(&page->lru, pagelist);
			inc_zone_page_state(page, NR_ISOLATED_ANON +
					    page_is_file_cache(page));
		}
	}
}

#if 0
static struct page *new_node_page(struct page *page, unsigned long node, int **x)
{
	return alloc_pages_exact_node(node, GFP_HIGHUSER_MOVABLE, 0);
}

int checkifexist(unsigned long pfn){

	int totpg = 4096;
	int idx =0;	
	//debug_pfn[4096];
	//debug_pfn_cnt[4096];
	
	for ( idx =0; idx < totpg; idx++) {
		if(debug_pfn[idx] == pfn)
			return idx;
	}
	return -1;
}

int debug_cnt;
int adddebugpfn(unsigned long pfn) {

	debug_pfn[debug_cnt]=pfn;
	debug_pfn_cnt[debug_cnt]++;
	debug_cnt++;
}

int addifexist(unsigned long pfn, int idx){

	 debug_pfn_cnt[idx]++;
	 //printk(KERN_ALERT "pfn:%lu,debug_pfn[%u]:%lu\n",
	//		 	pfn,idx,debug_pfn_cnt[idx]);
}
#endif

void print_all_conversion(struct vm_area_struct *vma, struct page *page){

   struct page *vmapg=NULL;	
   struct page *vmapgend=NULL;

	//for (vma =current->mm->mmap; vma; vma = vma->vm_next) {

	 pgd_t *pgd = pgd_offset(current->mm, vma->vm_start);
	 pmd_t *pmd = pmd_offset(pgd, vma->vm_start);  
	 pte_t *pte = pte_offset_map(pmd, vma->vm_start);  

	 pgd_t *pgd1 = pgd_offset(current->mm, vma->vm_end);
	 pmd_t *pmd1 = pmd_offset(pgd1, vma->vm_end);  
	 pte_t *pte1 = pte_offset_map(pmd1, vma->vm_end);  

		printk("page_to_pfn(page) %lu\n", page_to_pfn(page));
		printk("kmap(page) %lu\n",kmap(page));
		printk("vma->vm_start %lu\n",vma->vm_start);
		printk("vma->vm_end %lu\n",vma->vm_end);
		printk("vma->size %lu\n",vma->vm_end-vma->vm_start);

		printk("virt_to_phys(vma->vm_start) %lu\n",virt_to_phys(vma->vm_start));

    	//if (!pte_present(*pte)) 
		vmapg = pte_page(*pte);

	    //if (!pte_present(*pte)) 
		vmapgend = pte_page(*pte1);

		if(vmapg) {
			printk("page_to_pfn(vmapg) %lu\n", page_to_pfn(vmapg));
			printk("kmap(vmapg) %lu\n",kmap(vmapg));
		}

		if(vmapgend) {
			printk("page_to_pfn(vmapgend) %lu\n", page_to_pfn(vmapgend));
			printk("kmap(vmapgend) %lu\n",kmap(vmapgend));
		}



		printk("__pa(vma->vm_start) %lu\n",(unsigned long)__pa(vma->vm_start));
		vmapg = (struct page*)PAGE_ALIGN(vma->vm_start);
		if(vmapg){
			printk("page_to_pfn(vmapg) %lu\n", page_to_pfn(vmapg));
			 printk("kmap(vmapg) %lu\n",kmap(vmapg));	
		}
	//}
		printk("****************************\n");
	
}


#ifdef HETEROSTATS
int hetero_stats_addvmapg(struct vm_area_struct *vma, struct page *page){

	int cntr=0;

	for (cntr =0; cntr < vmacntr; cntr++) {

		struct vm_area_struct *vmatmp;		
		vmatmp = (struct vm_area_struct *)vma_list[cntr];

		if(vmatmp == vma) {
			vma_hotpgcnt[cntr]++;	
			return 0;
		}
	}
	return -1;
}

void hetero_print_vmastats(){

    int cntr=0;

	printk(KERN_ALERT "*****************\n");

    for (cntr =0; cntr < vmacntr; cntr++) {
	
		struct vm_area_struct *vmatmp;		
		vmatmp = (struct vm_area_struct *)vma_list[cntr];
   	
        printk(KERN_ALERT
               "\nvma[%d] vm_start %lu vm_end %lu "
			   "flag: %c%c%c%c%c%c  hotpagecnt %lu ", /// 0x%08lx000 ",
               cntr,
               (unsigned long)vmatmp->vm_start, (unsigned long)vmatmp->vm_end,
               vmatmp->vm_flags & VM_READ ? 'r' : '-',
               vmatmp->vm_flags & VM_WRITE ? 'w' : '-',
               vmatmp->vm_flags & VM_EXEC ? 'x' : '-',
               vmatmp->vm_flags & VM_MAYSHARE ? 's' : 'p',
               vmatmp->vm_flags & VM_LOCKED ? 'l' : '-',
               vmatmp->vm_flags & VM_IO ? 'i' : '-',
			   vma_hotpgcnt[cntr]);
               //vmatmp->vm_pgoff);
    }
	
	printk(KERN_ALERT "Hypervisor reported "
			"Num. hot pages %u\n",stat_xen_hot_pages);
	printk(KERN_ALERT "*****************\n");
}
#endif



/* Scan through pages checking if pages follow certain conditions. */
static int check_pte_range(struct vm_area_struct *vma, pmd_t *pmd,
		unsigned long addr, unsigned long end,
		const nodemask_t *nodes, unsigned long flags,
		void *private, int addtolist)
{
	pte_t *orig_pte;
	pte_t *pte;
	spinlock_t *ptl;

	orig_pte = pte = pte_offset_map_lock(vma->vm_mm, pmd, addr, &ptl);
	do {
		pg_debug_count++;

		struct page *page;
		//int nid;

		if (!pte_present(*pte))
			continue;

		page = vm_normal_page(vma, addr, *pte);
		if (!page)
			continue;

#ifdef HETEROSTATS
		normalpg_dbg_count++;
		stat_tot_proc_pages++;
#endif		

		/*
		 * vm_normal_page() filters out zero pages, but there might
		 * still be PageReserved pages to skip, perhaps in a VDSO.
		 */
		if (PageReserved(page))
			continue;

#ifndef ENABLE_HETERO
		nonrsrvpg_dbg_count++;
		nid = page_to_nid(page);
		if (node_isset(nid, *nodes) == !!(flags & MPOL_MF_INVERT))
			continue;

		beforemigrate_dbg_count++;	
#endif
		
#ifndef ENABLE_HETERO
		if (flags & (MPOL_MF_MOVE | MPOL_MF_MOVE_ALL)) {
#endif


#ifdef _DISABLE_HETERO_CHECK			
#else
        if(is_hetero_hot_page(page)) {
             //printk(KERN_ALERT "migrate_pages: page in hotlist \n");
         }else {
             continue;
         }
		 //print_all_conversion(vma, page);
		 if(page->nvdirty == PAGE_MIGRATED) {
			nr_dup_hot_page++;  
			//continue;
		  }	
#endif
		  /*page has been already added to the dirty list*/
		 page->nvdirty=PAGE_MIGRATED;

#ifdef HETEROSTATS
		 /*unique hot pages detected*/
		pageinhotlist++;
		hetero_stats_addvmapg(vma, page);
#endif
		if(addtolist)
			migrate_page_add(page, private, flags);

#ifndef ENABLE_HETERO
		}
		else
			break;
#endif

	} while (pte++, addr += PAGE_SIZE, addr != end);
	pte_unmap_unlock(orig_pte, ptl);
	//printk(KERN_ALERT "***********************\n");
	return addr != end;
}

static inline int check_pmd_range(struct vm_area_struct *vma, pud_t *pud,
		unsigned long addr, unsigned long end,
		const nodemask_t *nodes, unsigned long flags,
		void *private, int addtolist)
{
	pmd_t *pmd;
	unsigned long next;

	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		split_huge_page_pmd(vma, addr, pmd);
		if (pmd_none_or_trans_huge_or_clear_bad(pmd))
			continue;
		if (check_pte_range(vma, pmd, addr, next, nodes,
				    flags, private, addtolist))
			return -EIO;
	} while (pmd++, addr = next, addr != end);
	return 0;
}

static inline int check_pud_range(struct vm_area_struct *vma, pgd_t *pgd,
		unsigned long addr, unsigned long end,
		const nodemask_t *nodes, unsigned long flags,
		void *private, int addtolist)
{
	pud_t *pud;
	unsigned long next;

	pud = pud_offset(pgd, addr);
	do {
		next = pud_addr_end(addr, end);
		if (pud_none_or_clear_bad(pud))
			continue;
		if (check_pmd_range(vma, pud, addr, next, nodes,
				    flags, private, addtolist))
			return -EIO;
	} while (pud++, addr = next, addr != end);
	return 0;
}

static inline int check_pgd_range(struct vm_area_struct *vma,
		unsigned long addr, unsigned long end,
		const nodemask_t *nodes, unsigned long flags,
		void *private, int addtolist)
{
	pgd_t *pgd;
	unsigned long next;

	pgd = pgd_offset(vma->vm_mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none_or_clear_bad(pgd))
			continue;
		if (check_pud_range(vma, pgd, addr, next, nodes,
				    flags, private, addtolist))
			return -EIO;
	} while (pgd++, addr = next, addr != end);
	return 0;
}

/*
 * Check if all pages in a range are on a set of nodes.
 * If pagelist != NULL then isolate pages from the LRU and
 * put them on the pagelist.
 */
static struct vm_area_struct *
check_node_range(struct mm_struct *mm, unsigned long start, unsigned long end,
                const nodemask_t *nodes, unsigned long flags, void *private,
				int addtolist)
{
        int err;
        struct vm_area_struct *first, *vma, *prev;


        first = find_vma(mm, start);
        if (!first)
                return ERR_PTR(-EFAULT);
        prev = NULL;
        for (vma = first; vma && vma->vm_start < end; vma = vma->vm_next) {
                unsigned long endvma = vma->vm_end;

                if (endvma > end)
                        endvma = end;
                if (vma->vm_start > start)
                        start = vma->vm_start;

                if (!(flags & MPOL_MF_DISCONTIG_OK)) {
                        if (!vma->vm_next && vma->vm_end < end)
                                return ERR_PTR(-EFAULT);
                        if (prev && prev->vm_end < vma->vm_start)
                                return ERR_PTR(-EFAULT);
                }

                if (is_vm_hugetlb_page(vma))
                        goto next;

                if (flags & MPOL_MF_LAZY) {
                        change_prot_numa(vma, start, endvma);
                        goto next;
                }

                if ((flags & MPOL_MF_STRICT) ||
                     ((flags & (MPOL_MF_MOVE | MPOL_MF_MOVE_ALL)) &&
                      vma_migratable(vma))) {

                        err = check_pgd_range(vma, start, endvma, nodes,
                                                flags, private, addtolist);
                        if (err) {
                                first = ERR_PTR(err);
                                break;
                        }
                }
next:
                prev = vma;
        }
        return first;
}


#if 0
void print_all_vmas() {

	struct vm_area_struct *vma;
	struct mm_struct *mm = current->mm;
	int err;

	for (vma = mm->mmap; vma && !err; vma = vma->vm_next) {

		printk("print_all_vmas: vma->start %lu, vma->end %lu \n",
  	    	vma->vm_start, vma->vm_end);
	}
}
#endif

/*
 */
static int check_vma_exists(struct page *new, struct vm_area_struct *vma,
                 unsigned long addr, void *private)
{

	//struct mm_struct *mm;
	int flags = MPOL_MF_MOVE|MPOL_MF_MOVE_ALL| MPOL_MF_DISCONTIG_OK;

	if(vma){

		if(vma->vm_mm == current->mm){
			//printk(KERN_ALERT "valid page \n");
			migrate_page_add(new, private, flags);	
			pageinhotlist++;
			return 1;
		}
		else{
			return 0;
		}
	}

	return 0;
}

static int find_page_vma(struct page *new, void *private)
{
	int ret = 0;
	int page_was_unlocked=0;

	if(!new) return SWAP_AGAIN;

	if(!PageLocked(new)){
        lock_page(new);

		if(!PageLocked(new))	
			return SWAP_AGAIN;

        page_was_unlocked = 1;
    }

  	if(!page_count(new)){
		if(page_was_unlocked)
            unlock_page(new);
		return SWAP_AGAIN;
	}
        
    ret = rmap_walk(new, check_vma_exists, private);

	if(page_was_unlocked)
            unlock_page(new);

	return ret;
}	



static int migrate_hot_pages(struct page *page, void *private, int flags)
{
	int page_was_unlocked=0;
    int err = 0;
	
	if (!page){
		nr_invalid_page++;
		return -1;
	}

	err = PTR_ERR(page);
	if (IS_ERR(page)) {
		nr_invalid_page++;
		return -1;
   	}

	/*
	 * vm_normal_page() filters out zero pages, but there might
	 * still be PageReserved pages to skip, perhaps in a VDSO.
	 */
	if (PageReserved(page)){
		nr_invalid_page++;
		return -1;
	}

	//if(page_is_file_cache(page))
	//	return -1;

	if(!page_count(page))
		return -1;

	//err = isolate_lru_page(page);
	//if(err)
	//	return -1;

	 //print_all_conversion(vma, page);
	 //if(page->nvdirty == PAGE_MIGRATED) {
	//	nr_dup_hot_page++;  
	//	return -1;
	  //}	

	 ///*page has been already added to the dirty list*/
	 //Set only if page is migrated in my_migrate_pages.
	 //page->nvdirty=PAGE_MIGRATED;
#if 0
	if(!PageLocked(page)){
		lock_page(page);
		page_was_unlocked = 1;
	}
#endif

	 if(!find_page_vma(page, private)){
		nr_invalid_page++; 
#if 0
		if(page_was_unlocked)
			unlock_page(page);
#endif
		return -1;
	 }

#if 0
	if(page_was_unlocked) 
		unlock_page(page);
#endif
	nonrsrvpg_dbg_count++;

	return 0;
}


#ifdef HETEROSTATS
int do_complete_page_walk() {

	struct vm_area_struct * vma;
	struct mm_struct *mm = current->mm;
	unsigned long end=0;
	unsigned long start=0;
	int source =0;
	int flags=0;
	nodemask_t nmask;
	LIST_HEAD(walklist);	

	stat_tot_proc_pages=0;

	nodes_clear(nmask);
	node_set(source, nmask);

	for (vma = mm->mmap; vma; vma = vma->vm_next) {

	  if (!vma)
         return -1;

	 start = vma->vm_start;
	 end = vma->vm_end;	
		
	 if(end < start)
		return -1;

	  check_node_range(mm, start, end, &nmask,
    	  flags|MPOL_MF_MOVE|MPOL_MF_MOVE_ALL| MPOL_MF_DISCONTIG_OK, &walklist, 0);
	}
	return 0;
}
#endif





#ifdef DEBUG_TIMER
long simulation_time(struct timespec start, struct timespec end)
{
	long current_time;
		
	current_time = ((end.tv_sec * 1000000000 + end.tv_nsec) -
						(start.tv_sec *1000000000 + start.tv_nsec));
	return current_time;

}
#endif

int hetero_page_filter(struct page *page){

	//return 0;
	//if(test_bit(PG_nvram, &page->flags)){
	//	 return HETEROFAIL; 
	//}
	//if(test_bit(PG_swapbacked, &page->flags)) //&& test_bit(PG_lru,&page->flags))
	//if(test_bit(PG_active, &page->flags) && !test_bit(PG_dirty,&page->flags) 
	 //	&& !test_bit(PG_reserved,&page->flags) && !test_bit(PG_lru,&page->flags))
	if(test_bit(PG_active,&page->flags))
	{

#if 0
	  g_PG_swapbacked++;	

	  if(test_bit(PG_lru,&page->flags))
    	  g_PG_lru++;

	  if(test_bit(PG_active,&page->flags))
    	  g_PG_active++;

	  if(!test_bit(PG_active,&page->flags))
    	  g_PG_inactive++;

	  if(test_bit(PG_dirty,&page->flags))
    	  g_PG_dirty++;

	  if(test_bit(PG_private,&page->flags))
    	  g_PG_private++;

	  if(test_bit(PG_writeback,&page->flags))
    	  g_PG_writeback++;

	  if(test_bit(PG_reserved,&page->flags))
    	 g_PG_reserved++;

	  printk("g_PG_swapbacked %u, "
		 "g_PG_lru %u, "
		 "g_PG_active %u, "
		 "g_PG_inactive %u, "
		 "g_PG_dirty %u, "
		 "g_PG_private %u, "
		 "g_PG_writeback %u, "
		 "g_PG_reserved %u \n", 
		 g_PG_swapbacked,
		 g_PG_lru,
		 g_PG_active,
		 g_PG_inactive,
		 g_PG_dirty,
		 g_PG_private,
		 g_PG_writeback,
		 g_PG_reserved);
#endif
		return 0;
	}
	else {
		return HETEROFAIL;
	}

#if 0
	if(test_bit(PG_reserved, &page->flags))
		return HETEROFAIL;

	if(test_bit(PG_unevictable, &page->flags))
		return HETEROFAIL;

	if(test_bit(PG_writeback, &page->flags))
		return HETEROFAIL;

	if(test_bit(PG_mappedtodisk, &page->flags))
		return HETEROFAIL;

	if(test_bit(PG_reclaim, &page->flags))
		return HETEROFAIL;

	if(!test_bit(PG_active,&page->flags))
		return HETEROFAIL;	

	if(!test_bit(PG_swapcache,&page->flags))
		return HETEROFAIL;	

	return 0;	

	if(test_bit(PG_swapbacked, &page->flags))
			return HETEROFAIL;
#endif
}

int hetero_filter(struct vm_area_struct * vmatmp){

	 //char c;
	/*if(vmatmp->vm_flags & VM_EXEC)
		c ='x';
	else
		c ='-';*/

	 /*Executable*/	
	if(vmatmp && (vmatmp->vm_flags & VM_EXEC)) {
		//printk(KERN_ALERT "vmatmp->vm_flags & VM_EXEC %c\n", c);
		return 1;
	}

	 /*Read only*/
	if(vmatmp && (vmatmp->vm_flags & VM_READ) && !(vmatmp->vm_flags & VM_WRITE))
		return 1;


	 /*I/O specific*/	
	 if(vmatmp->vm_flags & VM_IO)
		return 1;	

	 return 0;
}


/*
 * Display pages allocated per node and memory policy via /proc.
 */
//static int show_numa_map(unsigned long start)
//SYSCALL_DEFINE1(move_inactpages, unsigned long, start)
asmlinkage long sys_move_inactpages(unsigned long start, unsigned long flag, 
									unsigned int hot_scan_freq, 
									unsigned int hot_scan_limit,
									unsigned int fastmemlimit,
									unsigned int usesharedmem,
									unsigned int hot_shrink_freq)

{

	struct vm_area_struct * vma;
	struct mm_struct *mm = current->mm;
	int source =0;
	int dest=1;
	int flags=0;
	int err =-1;
	unsigned int size=0;
	unsigned int hotpgcnt=0;
	unsigned long end=0;
	xen_pfn_t *hot_frame_list=NULL;
	nodemask_t nmask;
        LIST_HEAD(pagelist);
	//unsigned long migratetot=0;
	unsigned int cntr=0;
	int tmplock=0;

#ifdef DEBUG_TIMER
	unsigned long temp_time = 0;
	struct timespec start_mig_time, end_mig_time;
	struct timespec start_pgwalk, end_pgwalk;
	struct timespec start_hyercall,end_hyercall;
#endif //DEBUG_TIMER
	

    if (flag == HETERO_APP_INIT) {

<<<<<<< HEAD
 	if(current)
	  current->heteroflag = PF_HETEROMEM;
	printk(KERN_ALERT "sys_move_inactpages: calling sys_move_inactpages \n");
	//heteromem_app_enter(start);
	heteromem_app_enter(start,0,0,0,0,0);
	return nr_migrate_success;
     }
     start=0;
=======
 		if(current)
		  current->heteroflag = PF_HETEROMEM;

		nr_migrate_success = 0;

		heteromem_app_enter(start, 
							hot_scan_freq,
							hot_scan_limit,
							hot_shrink_freq,
							usesharedmem, 
							fastmemlimit);

		return nr_migrate_success;
	}
	start=0;
>>>>>>> b951cd246cd02adaf9dce69807bf314201a3cab8

	if (!mm)
		return 0;

	if(!flag) {
		printk(KERN_ALERT "sys_move_inactpages: invalid migration size \n");
	}

	nr_heteropgcnt=0;
	//init_hetero_list_fn();
	
#ifdef ENABLE_OS_MIGRATION	
	printk(KERN_ALERT "sys_move_inactpages: increment_hetero_alloc_miss \n");	
	increment_hetero_alloc_miss();
	return 0;
#endif

	INIT_LIST_HEAD(&pagelist);


#ifdef DEBUG_TIMER
        getnstimeofday (&start_hyercall);
#endif

#ifdef HETEROSTATS
        vmacntr=0;
#endif	

#if 1
	if(current)		
	current->heteroflag = PF_HETEROMEM;

<<<<<<< HEAD
	//hot_frame_list = get_hotpage_list(&hotpgcnt);
	//get_hotpage_list(&tmplock);
	hot_frame_list=get_hotpage_list_sharedmem(&hotpgcnt);
	//get_hotpage_list(&tmplock);
=======
	if(!usesharedmem){
		hot_frame_list = get_hotpage_list(&hotpgcnt);
	}else{
		hot_frame_list=get_hotpage_list_sharedmem(&hotpgcnt);
	}
>>>>>>> b951cd246cd02adaf9dce69807bf314201a3cab8
 	if( (hotpgcnt == 0) || !hot_frame_list || hotpgcnt < HOT_MIN_MIG_LIMIT) {
		return 0;
	}
	//printk(KERN_ALERT "hotcount %u\n",hotpgcnt);

#ifdef HETEROSTATS
		stat_xen_hot_pages = stat_xen_hot_pages + hotpgcnt;
#endif	


#ifdef DEBUG_TIMER
		temp_time = 0;
        getnstimeofday (&end_hyercall);
        temp_time = simulation_time(start_hyercall,end_hyercall);
        tot_hypercall_time = tot_hypercall_time + temp_time;
#endif

#ifdef DEBUG_TIMER
        getnstimeofday (&start_pgwalk);
		normalpg_dbg_count = 0;
		nr_dup_hot_page=0;	
		nr_invalid_page=0;
		nr_migrate_failed=0;
		nr_migrate_success=0;
		nr_migrate_retry=0;
		nonrsrvpg_dbg_count=0;
		nr_migrate_attempted=0;
		nr_page_freed=0;	
		nr_alloc_fail=0;
		nr_invalid_page=0;
		nr_incorrect_pg=0;
#endif

	for (cntr=0; cntr < hotpgcnt; cntr++){

		unsigned long mfn=0, pfn=0;
		struct page *page=NULL;

		mfn = hot_frame_list[cntr];
		if(!mfn) {
			nr_incorrect_pg++;
			continue;
		}

    	pfn =  mfn_to_local_pfn(mfn);
		if(!pfn){
			nr_incorrect_pg++;
			continue;
		}

	    page = pfn_to_page(pfn);
    	if(!page) {
			nr_incorrect_pg++;	
			continue;
		}

		 //print_all_conversion(vma, page);
	    if(page->nvdirty == PAGE_MIGRATED) {
    	    nr_dup_hot_page++;
        	continue;
      	}

		/*Filter certain kind of pages*/	
		//if(hetero_page_filter(page) == HETEROFAIL)
		//	continue;
		 //if (!PageAnon(page))
			//continue;

		flags = flags|MPOL_MF_MOVE|MPOL_MF_MOVE_ALL| MPOL_MF_DISCONTIG_OK;
		migrate_hot_pages(page, &pagelist,flags);
		//migrate_hot_pages(page, &hetero_list,flags);
	}

#ifdef DEBUG_TIMER
        temp_time=0;
        getnstimeofday (&end_pgwalk);
        temp_time = simulation_time(start_pgwalk,end_pgwalk);
        tot_pgwalk_time = tot_pgwalk_time + temp_time;
#endif

#ifdef DEBUG_TIMER
    getnstimeofday (&start_mig_time);
#endif
	
	if(nonrsrvpg_dbg_count){
		//printk("nonrsrvpg_dbg_count: %u \n",nonrsrvpg_dbg_count);	
		//return 0;
	}
	nr_curr_migrate_success = 0;

	if (!list_empty(&pagelist)) {
		spin_lock(&migrate_lock);
	  	err = my_migrate_pages(&pagelist, new_page_node, dest,
        	                      MIGRATE_SYNC, MR_SYSCALL);
        if (err) {
          putback_lru_pages(&pagelist);
	    }else {
  	  	}
		spin_unlock(&migrate_lock);
	}
	//if(nr_migrate_success)
	//printk("nr_migrate_success: %u \n",nr_migrate_success);

	/*if((nr_migrate_success % 10 == 0) || 
			(nr_migrate_success % 2 == 0)) //||nr_migrate_attempted || nr_migrate_failed)
		printk("nr_migrate_success: %u "
				"nr_migrate_failed: %u "
				"nr_migrate_attempted: %u\n",
				nr_migrate_success, 
				nr_migrate_failed,
				nr_migrate_attempted); */

	/*printk("cntr %u, nr_migrate_success: %u "
			"nr_migrate_failed: %u, nr_incorrect_pg: %u "
			"nr_migrate_retry: %u "
			"nr_migrate_attempted: %u "
			"nonrsrvpg_dbg_count: %u "
			"nr_page_freed: %u, nr_alloc_fail: %u "
			"nr_dup_hot_page %u, nr_invalid_page %u\n", 
			cntr, nr_migrate_success, nr_migrate_failed,
			nr_incorrect_pg, nr_migrate_retry, nr_migrate_attempted,
			nonrsrvpg_dbg_count, nr_page_freed, nr_alloc_fail,
			nr_dup_hot_page, nr_invalid_page);*/

#ifdef DEBUG_TIMER
	cntr=0;
    temp_time = 0;
    getnstimeofday (&end_mig_time);
    temp_time = simulation_time(start_mig_time,end_mig_time);
    tot_mig_time = tot_mig_time + temp_time;
    printk("tot_mig_time %lu ns \n",tot_mig_time);
    printk("tot_pgwalk_time %lu ns \n", tot_pgwalk_time);
#endif

#ifdef HETEROSTATS
    /*if(num_migrated < nr_migrate_success){
        hetero_print_vmastats();
        num_migrated = nr_migrate_success;
		//do_complete_page_walk();
    }*/

	if( nr_migrate_success % 10 == 0)
	printk(KERN_ALERT "sys_move_inactpages: %u out of %u, "
              	"pageinhotlist %u, nr_dup_hot_page %u "
			 	"Total Proc Page %u, num_unmap_pages %u, "
				"num_pages_before_move %u, "
				"nr_migrate_failed %u ,"
				"nr_migrate_retry %u, "
				"nr_page_freed %u "
				"nr_migrate_attempted %u\n",
              	nr_migrate_success,size, 
			  	pageinhotlist, nr_dup_hot_page,
				stat_tot_proc_pages,
				num_unmap_pages,
				num_pages_before_move,
				nr_migrate_failed, 
				nr_migrate_retry,
				nr_page_freed,
				nr_migrate_attempted);
#endif

	/*total hot page count*/
	return nr_curr_migrate_success;
#endif

#if 0
	for (vma = mm->mmap; vma; vma = vma->vm_next) {

	    if (!vma)
	      goto out_plug;

		start = vma->vm_start;
		end = vma->vm_end;


#ifdef _DISABLE_HETERO_CHECK
#else
		//filter vmas to optimize page walks	
		if(hetero_filter(vma))
			continue;	
#endif


#ifdef HETEROSTATS
		 vma_list[vmacntr]= (unsigned long)vma;
		 vmacntr++;
#endif

#if 0	
	while(migratetot < flag) {

	//vma = find_vma(current->mm, start);
	if(vma) {
		start = vma->vm_start;
		end = vma->vm_end;
		printk("sys_move_inactpages: user supplied address range \n");
	}else {
		start = mm->mmap->vm_start;
		end = mm->mmap->vm_start +  mm->task_size;
		printk("sys_move_inactpages: using whole application range \n");
	}
	migratetot += end-start;
#endif

	//printk("sys_move_inactpages: calling check_node_range " 
	//		" start %lu, end %lu, size %lu\n", start,end, end-start);
    check_node_range(mm, start, end, &nmask,
                    flags|MPOL_MF_MOVE|MPOL_MF_MOVE_ALL| MPOL_MF_DISCONTIG_OK, &pagelist, 1);

#ifdef DEBUG_TIMER
		temp_time=0;
	    getnstimeofday (&end_pgwalk);
		temp_time = simulation_time(start_pgwalk,end_pgwalk);
		tot_pgwalk_time = tot_pgwalk_time + temp_time;
#endif

#ifdef DEBUG_TIMER
	getnstimeofday (&start_mig_time);
#endif
  	  if (!list_empty(&pagelist)) {
            err = my_migrate_pages(&pagelist, new_page_node, dest,
                                    MIGRATE_SYNC, MR_SYSCALL);
            if (err)
              putback_lru_pages(&pagelist);
    	}else {
		}
#ifdef DEBUG_TIMER
	temp_time = 0;	
	getnstimeofday (&end_mig_time);
	temp_time = simulation_time(start_mig_time,end_mig_time);
	tot_mig_time = tot_mig_time + temp_time;
	printk("tot_mig_time %lu \n",tot_mig_time);
	printk("tot_pgwalk_time %lu \n", tot_pgwalk_time);
#endif

#ifdef HETEROSTATS
	if(num_migrated < nr_migrate_success){
		hetero_print_vmastats();
		num_migrated = nr_migrate_success;
	}
#endif

	 if(nr_migrate_success % 1000 == 0)
	 printk(KERN_ALERT "sys_move_inactpages: %u out of %u, "
               "pageinhotlist %u, nr_dup_hot_page %u\n",
              nr_migrate_success,size, 
			  pageinhotlist, nr_dup_hot_page);
	}
out_plug:
	//printk(KERN_ALERT "Finished walking the process pagetree \n");
	return 0;
#endif

#if 0
//#ifdef SELECTIVE_HETERO_SCAN
	for (vma = mm->mmap; vma; vma = vma->vm_next) {

	  if (!vma)
         goto out_plug;

		 start = vma->vm_start;
		 end = vma->vm_end;	
		
		 if(end < start)
			goto out_plug;

		  check_node_range(mm, start, end, &nmask,
    			  flags|MPOL_MF_MOVE|MPOL_MF_MOVE_ALL| MPOL_MF_DISCONTIG_OK, &pagelist);
	}


	hot_frame_list = get_hotpage_list(&hotpgcnt);
 	if(!hotpgcnt || !hot_frame_list) {
		//printk(KERN_ALERT "sys_move_inactpages: "
		//		"hot_frame_list %lu, hotpgcnt %u \n",
		//		(unsigned long)hot_frame_list, hotpgcnt);
		return 0;
	}


	for (cntr=0; cntr < hotpgcnt; cntr++){

   		unsigned long pfn =  mfn_to_local_pfn(hot_frame_list[cntr]);
		struct page *page = pfn_to_page(pfn);

		if(!page) continue;
	
		if(page->nvdirty == PAGE_MIGRATED)
			  continue;

		for (vma = mm->mmap; vma; vma = vma->vm_next) {
			/*Check if page is set to vm_start*/	
			if(vma && (vma->vm_start == page->nvpgoff))
				goto check_migrate;
		}

		 if (!vma)
       		continue;

check_migrate:		 
		  check_node_range(mm, vma->vm_start, vma->vm_end, &nmask,
    			  flags|MPOL_MF_MOVE|MPOL_MF_MOVE_ALL| MPOL_MF_DISCONTIG_OK, 
				  &pagelist, 1);

#ifdef HETERODEBUG
		  printk(KERN_ALERT "sys_move_inactpages: after check_node_range pg_debug_count %u, "
    	      "nonrsrvpg_dbg_count %u, beforemigrate_dbg_count %u, "
        	  "normalpg_dbg_count %u\n", pg_debug_count, nonrsrvpg_dbg_count, 
	       	   beforemigrate_dbg_count, normalpg_dbg_count);
#endif

	      /*if (!list_empty(&pagelist)) {
    	   	 //printk("sys_move_inactpages: calling migrate_pages function \n");
		     err = my_migrate_pages(&pagelist, new_page_node, dest,
                               MIGRATE_SYNC, MR_SYSCALL);
    	    if (err)
            	 putback_lru_pages(&pagelist);
		  }else {
		  }*/
		 size += (end-start)/4096;	
		 printk(KERN_ALERT "sys_move_inactpages: %u out of %u, "
				 	"pageinhotlist %u, nr_dup_hot_page %u\n",
                    nr_migrate_success,size, 
					pageinhotlist, nr_dup_hot_page);

    }
#endif

//#ifdef ENABLE_HETERO
//	return nr_heteropgcnt;
//#else
  //  return err;
//#endif
	//iterate_all_lrulist();
	//return 0;

#if 0
#ifdef HETERODEBUG
	printk("Calling move_inactpages,start addr %lu \n",start);	
#endif
	md = kmalloc(sizeof(struct migrate_maps), GFP_KERNEL);
	if(!md) {
		printk(KERN_ALERT "md alloc failed \n");
		return 0;
	}

	/* Ensure we start with an empty set of numa_maps statistics. */
	memset(md, 0, sizeof(*md));

	vma = find_vma_prev(current->mm, start, &prev);
	start = vma->vm_start +1;

    if (vma && start > vma->vm_start)
        prev = vma;

	 for (;;) {

   	 if (!vma)
        	 goto out_plug;

#ifdef HETERODEBUG
	 printk("Iterating vma->vm_start: %lu,"
			 "vma->vm_end: %lu, start: %lu,end: %lu \n", vma->vm_start, 
			 vma->vm_end,  start, end);
#endif

     /* Here start < (end|vma->vm_end). */
     if (start < vma->vm_start) {
         start = vma->vm_start;
         if (start >= end)
             goto out_plug;
     }

     /* Here vma->vm_start <= start < (end|vma->vm_end) */
     tmp = vma->vm_end;
     if (end < tmp)
         tmp = end;

	walk.mm = mm;
	walk.pmd_entry = mig_gather_pte_stats;
	walk.private = md;

//#ifdef HETERODEBUG
	 printk("invoking walk_page_range "
			"vma->vm_start: %lu, vma->vm_end: %lu \n",
			 vma->vm_start,  vma->vm_end);
//#endif

	walk_page_range(vma->vm_start, vma->vm_end, &walk);

     start = tmp;
     if (prev && start < prev->vm_end)
         start = prev->vm_end;

     if (start >= end)
         goto out_plug;

     if (prev)
         vma = prev->vm_next;
     else    /* madvise_remove dropped mmap_sem */
         vma = find_vma(current->mm, start);
	}
#endif
//out_plug:
	//hetero_do_pages_move(current->mm, inactpgcnt, 0, &mylist);
//	printk(KERN_ALERT "Finished walking the process pagetree \n");
//	return 0;
}


#if 0
/*
 * Move a list of pages in the address space of the currently executing
 * process.
 */
SYSCALL_DEFINE6(migrate_inactive_pages, pid_t, pid, unsigned long, nr_pages,
		unsigned long, startaddr,
		const int __user *, nodes,
		int __user *, status, int, flags)
{
	const struct cred *cred = current_cred(), *tcred;
	struct task_struct *task;
	struct mm_struct *mm;
	int err;
	nodemask_t task_nodes;

	/* Check flags */
	if (flags & ~(MPOL_MF_MOVE|MPOL_MF_MOVE_ALL))
		return -EINVAL;

	if ((flags & MPOL_MF_MOVE_ALL) && !capable(CAP_SYS_NICE))
		return -EPERM;

	/* Find the mm_struct */
	rcu_read_lock();
	task = pid ? find_task_by_vpid(pid) : current;
	if (!task) {
		rcu_read_unlock();
		return -ESRCH;
	}
	get_task_struct(task);

	/*
	 * Check if this process has the right to modify the specified
	 * process. The right exists if the process has administrative
	 * capabilities, superuser privileges or the same
	 * userid as the target process.
	 */
	tcred = __task_cred(task);
	if (!uid_eq(cred->euid, tcred->suid) && !uid_eq(cred->euid, tcred->uid) &&
	    !uid_eq(cred->uid,  tcred->suid) && !uid_eq(cred->uid,  tcred->uid) &&
	    !capable(CAP_SYS_NICE)) {
		rcu_read_unlock();
		err = -EPERM;
		goto out;
	}
	rcu_read_unlock();

 	err = security_task_movememory(task);
 	if (err)
		goto out;

	task_nodes = cpuset_mems_allowed(task);
	mm = get_task_mm(task);
	put_task_struct(task);

	if (!mm)
		return -EINVAL;

	if (nodes) {

	}
	/*	err = do_pages_move(mm, task_nodes, nr_pages, pages,
				    nodes, status, flags);
	else
		err = do_pages_stat(mm, nr_pages, pages, status);*/

	mmput(mm);
	return err;

out:
	put_task_struct(task);
	return err;
}
#endif

#endif /* CONFIG_NUMA_BALANCING */


#endif /* CONFIG_NUMA */


		/*unsigned long temp = (unsigned long)(__va(pfn));
		void *virtaddr = (void *)(temp << PAGE_SHIFT);	
		void *kvirtaddr = kmap(page);
		struct page *vmapg, *vmendpg;
		unsigned long vmapfn; // = virt_to_phys(vma->vm_start);*/


		//if(page->nvpgoff || page->index){
		// }else {
		//	 continue;
		 //}
		//continue;
		//
		//for (vma = mm->mmap; vma; vma = vma->vm_next) {
#if 0
		    /*pgd_t *pgd = pgd_offset(mm, vma->vm_start);
			pmd_t *pmd = pmd_offset(pgd, vma->vm_start);  
			pte_t *pte = pte_offset_map(pmd, vma->vm_start);  

	 		//if (!pte_present(*pte))
		      //continue;

			vmapg = pte_page(*pte);

			pgd = pgd_offset(mm, vma->vm_end);
			pmd = pmd_offset(pgd, vma->vm_end);  
			pte = pte_offset_map(pmd, vma->vm_end);  
			vmendpg = pte_page(*pte);

	  		//if (!pte_present(*pte))
		      // continue;
	 		

			if(vmapg && vmendpg)*/

			vmapfn =(unsigned long)__pa(vma->vm_start);

			page = (struct page*)PAGE_ALIGN(vma->vm_start);

			printk(KERN_ALERT "hotpfn %lu, vmapfn %lu "
					 "vma->vm_start %lu, kmap(vmastart) %lu "
					 "kvirtaddr %lu \n",
					(unsigned long)pfn, 
					(unsigned long)vmapfn, 
					(unsigned long)vma->vm_start,
					(unsigned long)kmap(page),
					(unsigned long)kvirtaddr);

			continue;
		}
#endif


static int migrate_lru_pages(struct page *page, void *private, int flags)
{
	int page_was_unlocked=0;
    int err = 0;
	struct task_struct *task = current;
	struct mm_struct *mm;
	
	if (!page){
		nr_invalid_page++;
		return -1;
	}

	if(task)
		mm = task->mm;

	if(mm)
	down_read(&mm->mmap_sem);

	//printk(KERN_ALERT "migrate_lru_pages called \n");

	err = PTR_ERR(page);
	if (IS_ERR(page)) {
		nr_invalid_page++;
		goto err_migration;
   	}

	/*
	 * vm_normal_page() filters out zero pages, but there might
	 * still be PageReserved pages to skip, perhaps in a VDSO.
	 */
	//if (PageReserved(page)){
	//	nr_invalid_page++;
	//	return -1;
	//}

	//if(page_is_file_cache(page))
	//	return -1;

	//if(!page_count(page))
	//	return -1;

	//err = isolate_lru_page(page);
	//if(err)
	//	return -1;

	 //print_all_conversion(vma, page);
	 //if(page->nvdirty == PAGE_MIGRATED) {
	//	nr_dup_hot_page++;  
	//	return -1;
	  //}	

	 ///*page has been already added to the dirty list*/
	 //Set only if page is migrated in my_migrate_pages.
	 //page->nvdirty=PAGE_MIGRATED;
#if 0
	if(!PageLocked(page)){
		lock_page(page);
		page_was_unlocked = 1;
	}
#endif

	 if(!find_page_vma(page, private)){
		nr_invalid_page++; 
#if 0
		if(page_was_unlocked)
			unlock_page(page);
#endif
		goto err_migration;
	 }

#if 0
	if(page_was_unlocked) 
		unlock_page(page);
#endif
	nonrsrvpg_dbg_count++;

	if(mm)
	up_read(&mm->mmap_sem);
	return 0;

err_migration:
	if(mm)
	up_read(&mm->mmap_sem);
	return -1;

}



int  release_inactive_fastmem(struct list_head *pagelist, int maxattempt)
{
  int dest=1;
  int err=-1; 	
  int success=0;	
  int flags = 0;
  struct page *page=NULL;
  struct page *page2=NULL;
  int iterated_pages =0;
  int page_was_unlocked = 0;

  LIST_HEAD(fastpagelist);	
  INIT_LIST_HEAD(&fastpagelist);

   flags = flags|MPOL_MF_MOVE|MPOL_MF_MOVE_ALL| MPOL_MF_DISCONTIG_OK;

   drain_all_pages();
   return 0;

#if 1
   //spin_lock(&migrate_slowmem_lock);
	list_for_each_entry_safe(page, page2,pagelist, nvlist) {

		if(iterated_pages  > maxattempt)
			break;

		iterated_pages++;

		int page_was_unlocked = 0;
		//if(page && PageAnon(page) && !PageUnevictable(page)) /*working for all except graphchi*/
	    //if(page && !PageDirty(page) && !PageUnevictable(page)) /*works at times for graphchi*/
		//if(page && !PageUnevictable(page) && !PageActive(page)){
		//if(page && PageLRU(page) && !PageUnevictable(page)){
		//if(page && !PageAnon(page) && !PageUnevictable(page) && !PageActive(page) ) {
		//if(page && !PageAnon(page) && !PageActive(page) ) {
		//if(page && !PageUnevictable(page) && !PageActive(page)){
		if(page){
			//if(PageAnon(page))
	 		 //migrate_hot_pages(page, &fastpagelist,flags);
			 migrate_lru_pages(page, &fastpagelist,flags);
		}else {
			//page->nvpgoff++;
		}
	}
	//if(nonrsrvpg_dbg_count)
	//printk(KERN_ALERT "iterated_pages %u \n", iterated_pages);	
	//return 0;	
	//spin_unlock(&migrate_slowmem_lock); 	
#endif 

  if (!list_empty(&fastpagelist)) {

        spin_lock(&migrate_slowmem_lock);
		
        err = migrate_nearmem_pages(&fastpagelist, new_slowpage, dest,
                                  MIGRATE_SYNC, MR_SYSCALL, maxattempt);
        if (err) {
          putback_lru_pages(&fastpagelist);
        }else {
			success++;
        }
        spin_unlock(&migrate_slowmem_lock);

		printk(KERN_ALERT "nonrsrvpg_dbg_count %u %u %u\n", 
				nonrsrvpg_dbg_count, iterated_pages, nr_reclaim_success);
    }else {
		printk(KERN_ALERT "LIST EMPTY: nonrsrvpg_dbg_count %u %u\n", nonrsrvpg_dbg_count, iterated_pages);
	}
	return success;
}
EXPORT_SYMBOL(release_inactive_fastmem);


