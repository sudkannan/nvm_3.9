#define NUM_PAGES 16

struct frame {
	    unsigned int mfn;
};

xen_pfn_t *get_hotpage_list_sharedmem(unsigned int *hotcnt);
int reset_perf_counters(void);
int get_perf_counters(void);
int print_perf_counters(void);
int MigrationEnable(void);
<<<<<<< HEAD
void perf_set_test_arg(unsigned long arg);
=======
void perf_set_test_arg(unsigned long testarg,
                         unsigned int hot_scan_freq,
                         unsigned int hot_scan_limit,
                         unsigned int hot_shrink_freq,
                         unsigned int usesharedmem);
>>>>>>> b951cd246cd02adaf9dce69807bf314201a3cab8
