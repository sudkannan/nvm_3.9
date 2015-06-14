#define NUM_PAGES 16

struct frame {
	    unsigned int mfn;
};

xen_pfn_t *get_hotpage_list_sharedmem(unsigned int *hotcnt);
int reset_perf_counters(void);
int get_perf_counters(void);
int print_perf_counters(void);
int MigrationEnable(void);
void perf_set_test_arg(unsigned long arg);
