/******************************************************************************
 * Xen heteromem functionality
 */

#define RETRY_UNLIMITED	0

struct heteromem_stats {
	/* We aim for 'current allocation' == 'target allocation'. */
	unsigned long current_pages;
	unsigned long target_pages;
	/* Number of pages in high- and low-memory heteromems. */
	unsigned long heteromem_low;
	unsigned long heteromem_high;
	unsigned long schedule_delay;
	unsigned long max_schedule_delay;
	unsigned long retry_count;
	unsigned long max_retry_count;
#ifdef CONFIG_XEN_BALLOON_MEMORY_HOTPLUG
	unsigned long hotplug_pages;
	unsigned long heteromem_hotplug;
#endif
};

extern struct heteromem_stats heteromem_stats;

void heteromem_set_new_target(unsigned long target);

int alloc_xenheteromemed_pages(int nr_pages, struct page **pages,
		bool highmem, int delpage);
void free_xenheteromemed_pages(int nr_pages, struct page **pages);

int heteromem_init(int idx, unsigned long start, unsigned long size);

/*HeteroMem Get next page*/
struct page *hetero_getnxt_page(bool prefer_highmem);

int send_hotpage_skiplist();

int get_hotpage_list();

struct device;
#ifdef CONFIG_XEN_SELFBALLOONING
extern int register_xen_selfheteromeming(struct device *dev);
#else
static inline int register_xen_selfheteromeming(struct device *dev)
{
	return -ENOSYS;
}
#endif
