#define NUM_PAGES 16

struct frame {
	    unsigned int mfn;
};

xen_pfn_t *get_hotpage_list_sharedmem(unsigned int *hotcnt);
