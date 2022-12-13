// SPDX-License-Identifier: GPL-2.0
/*
 * KMSAN error reporting routines.
 *
 * Copyright (C) 2019-2022 Google LLC
 * Author: Alexander Potapenko <glider@google.com>
 *
 */

#include <linux/console.h>
#include <linux/moduleparam.h>
#include <linux/stackdepot.h>
#include <linux/stacktrace.h>
#include <linux/uaccess.h>

#include "kmsan.h"

static DEFINE_RAW_SPINLOCK(kmsan_report_lock);
#define DESCR_SIZE 128
/* Protected by kmsan_report_lock */
static char report_local_descr[DESCR_SIZE];
int panic_on_kmsan __read_mostly;

#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "kmsan."
module_param_named(panic, panic_on_kmsan, int, 0);

/*
 * Skip internal KMSAN frames.
 */
static int get_stack_skipnr(const unsigned long stack_entries[],
			    int num_entries)
{
	int len, skip;
	char buf[64];

	for (skip = 0; skip < num_entries; ++skip) {
		len = scnprintf(buf, sizeof(buf), "%ps",
				(void *)stack_entries[skip]);

		/* Never show __msan_* or kmsan_* functions. */
		if ((strnstr(buf, "__msan_", len) == buf) ||
		    (strnstr(buf, "kmsan_", len) == buf))
			continue;

		/*
		 * No match for runtime functions -- @skip entries to skip to
		 * get to first frame of interest.
		 */
		break;
	}

	return skip;
}

/*
 * Currently the descriptions of locals generated by Clang look as follows:
 *   ----local_name@function_name
 * We want to print only the name of the local, as other information in that
 * description can be confusing.
 * The meaningful part of the description is copied to a global buffer to avoid
 * allocating memory.
 */
static char *pretty_descr(char *descr)
{
	int pos = 0, len = strlen(descr);

	for (int i = 0; i < len; i++) {
		if (descr[i] == '@')
			break;
		if (descr[i] == '-')
			continue;
		report_local_descr[pos] = descr[i];
		if (pos + 1 == DESCR_SIZE)
			break;
		pos++;
	}
	report_local_descr[pos] = 0;
	return report_local_descr;
}

void kmsan_print_origin(depot_stack_handle_t origin)
{
	unsigned long *entries = NULL, *chained_entries = NULL;
	unsigned int nr_entries, chained_nr_entries, skipnr;
	void *pc1 = NULL, *pc2 = NULL;
	depot_stack_handle_t head;
	unsigned long magic;
	char *descr = NULL;
	unsigned int depth;

	if (!origin)
		return;

	while (true) {
		nr_entries = stack_depot_fetch(origin, &entries);
		depth = kmsan_depth_from_eb(stack_depot_get_extra_bits(origin));
		magic = nr_entries ? entries[0] : 0;
		if ((nr_entries == 4) && (magic == KMSAN_ALLOCA_MAGIC_ORIGIN)) {
			descr = (char *)entries[1];
			pc1 = (void *)entries[2];
			pc2 = (void *)entries[3];
			pr_err("Local variable %s created at:\n",
			       pretty_descr(descr));
			if (pc1)
				pr_err(" %pSb\n", pc1);
			if (pc2)
				pr_err(" %pSb\n", pc2);
			break;
		}
		if ((nr_entries == 3) && (magic == KMSAN_CHAIN_MAGIC_ORIGIN)) {
			/*
			 * Origin chains deeper than KMSAN_MAX_ORIGIN_DEPTH are
			 * not stored, so the output may be incomplete.
			 */
			if (depth == KMSAN_MAX_ORIGIN_DEPTH)
				pr_err("<Zero or more stacks not recorded to save memory>\n\n");
			head = entries[1];
			origin = entries[2];
			pr_err("Uninit was stored to memory at:\n");
			chained_nr_entries =
				stack_depot_fetch(head, &chained_entries);
			kmsan_internal_unpoison_memory(
				chained_entries,
				chained_nr_entries * sizeof(*chained_entries),
				/*checked*/ false);
			skipnr = get_stack_skipnr(chained_entries,
						  chained_nr_entries);
			stack_trace_print(chained_entries + skipnr,
					  chained_nr_entries - skipnr, 0);
			pr_err("\n");
			continue;
		}
		pr_err("Uninit was created at:\n");
		if (nr_entries) {
			skipnr = get_stack_skipnr(entries, nr_entries);
			stack_trace_print(entries + skipnr, nr_entries - skipnr,
					  0);
		} else {
			pr_err("(stack is not available)\n");
		}
		break;
	}
}

void kmsan_report(depot_stack_handle_t origin, void *address, int size,
		  int off_first, int off_last, const void *user_addr,
		  enum kmsan_bug_reason reason)
{
	unsigned long stack_entries[KMSAN_STACK_DEPTH];
	int num_stack_entries, skipnr;
	char *bug_type = NULL;
	unsigned long ua_flags;
	bool is_uaf;

	if (!kmsan_enabled)
		return;
	if (!current->kmsan_ctx.allow_reporting)
		return;
	if (!origin)
		return;

	current->kmsan_ctx.allow_reporting = false;
	ua_flags = user_access_save();
	raw_spin_lock(&kmsan_report_lock);
	pr_err("=====================================================\n");
	is_uaf = kmsan_uaf_from_eb(stack_depot_get_extra_bits(origin));
	switch (reason) {
	case REASON_ANY:
		bug_type = is_uaf ? "use-after-free" : "uninit-value";
		break;
	case REASON_COPY_TO_USER:
		bug_type = is_uaf ? "kernel-infoleak-after-free" :
				    "kernel-infoleak";
		break;
	case REASON_SUBMIT_URB:
		bug_type = is_uaf ? "kernel-usb-infoleak-after-free" :
				    "kernel-usb-infoleak";
		break;
	case REASON_NET:
		bug_type = is_uaf ? "kernel-network-infoleak-after-free" :
				    "kernel-network-infoleak";
		break;
	}

	num_stack_entries =
		stack_trace_save(stack_entries, KMSAN_STACK_DEPTH, 1);
	skipnr = get_stack_skipnr(stack_entries, num_stack_entries);

	pr_err("BUG: KMSAN: %s in %pSb\n", bug_type,
	       (void *)stack_entries[skipnr]);
	stack_trace_print(stack_entries + skipnr, num_stack_entries - skipnr,
			  0);
	pr_err("\n");

	kmsan_print_origin(origin);

	if (size) {
		pr_err("\n");
		if (off_first == off_last)
			pr_err("Byte %d of %d is uninitialized\n", off_first,
			       size);
		else
			pr_err("Bytes %d-%d of %d are uninitialized\n",
			       off_first, off_last, size);
	}
	if (address)
		pr_err("Memory access of size %d starts at %px\n", size,
		       address);
	if (user_addr && reason == REASON_COPY_TO_USER)
		pr_err("Data copied to user address %px\n", user_addr);
	pr_err("\n");
	dump_stack_print_info(KERN_ERR);
	pr_err("=====================================================\n");
	add_taint(TAINT_BAD_PAGE, LOCKDEP_NOW_UNRELIABLE);
	raw_spin_unlock(&kmsan_report_lock);
	if (panic_on_kmsan)
		panic("kmsan.panic set ...\n");
	user_access_restore(ua_flags);
	current->kmsan_ctx.allow_reporting = true;
}
