/*
 * Copyright (C) 2008-2011 Mathieu Desnoyers
 * Copyright (C) 2009 Pierre-Marc Fournier
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Ported to userspace by Pierre-Marc Fournier.
 */

#define _LGPL_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include <urcu/arch.h>
#include <lttng/urcu/urcu-ust.h>
#include <urcu/hlist.h>
#include <urcu/uatomic.h>
#include <urcu/compiler.h>
#include <urcu/system.h>

#include <lttng/tracepoint.h>
#include <lttng/ust-abi.h>	/* for LTTNG_UST_SYM_NAME_LEN */

#include <usterr-signal-safe.h>
#include <helper.h>

#include "tracepoint-internal.h"
#include "lttng-tracer-core.h"
#include "jhash.h"
#include "error.h"

/* Test compiler support for weak symbols with hidden visibility. */
int __tracepoint_test_symbol1 __attribute__((weak, visibility("hidden")));
void *__tracepoint_test_symbol2 __attribute__((weak, visibility("hidden")));
struct {
	char a[24];
} __tracepoint_test_symbol3 __attribute__((weak, visibility("hidden")));

/* Set to 1 to enable tracepoint debug output */
static const int tracepoint_debug;
static int initialized;

/*
 * If tracepoint_destructors_state = 1, tracepoint destructors are
 * enabled. They are disabled otherwise.
 */
static int tracepoint_destructors_state = 1;

/*
 * Expose the now deprecated symbol __tracepoints__disable_destructors for
 * backward compatibility of applications built against old versions of
 * lttng-ust. We need to keep __tracepoints__disable_destructors up to date
 * within the new destructor disabling API because old applications read this
 * symbol directly.
 */
int __tracepoints__disable_destructors __attribute__((weak));

static void (*new_tracepoint_cb)(struct lttng_ust_tracepoint *);

/*
 * tracepoint_mutex nests inside UST mutex.
 *
 * Note about interaction with fork/clone: UST does not hold the
 * tracepoint mutex across fork/clone because it is either:
 * - nested within UST mutex, in which case holding the UST mutex across
 *   fork/clone suffice,
 * - taken by a library constructor, which should never race with a
 *   fork/clone if the application is expected to continue running with
 *   the same memory layout (no following exec()).
 */
static pthread_mutex_t tracepoint_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * libraries that contain tracepoints (struct tracepoint_lib).
 * Protected by tracepoint mutex.
 */
static CDS_LIST_HEAD(libs);

/*
 * The tracepoint mutex protects the library tracepoints, the hash table, and
 * the library list.
 * All calls to the tracepoint API must be protected by the tracepoint mutex,
 * excepts calls to tracepoint_register_lib and
 * tracepoint_unregister_lib, which take the tracepoint mutex themselves.
 */

/*
 * Tracepoint hash table, containing the active tracepoints.
 * Protected by tracepoint mutex.
 */
#define TRACEPOINT_HASH_BITS 12
#define TRACEPOINT_TABLE_SIZE (1 << TRACEPOINT_HASH_BITS)
static struct cds_hlist_head tracepoint_table[TRACEPOINT_TABLE_SIZE];

static CDS_LIST_HEAD(old_probes);
static int need_update;

static CDS_LIST_HEAD(release_queue);
static int release_queue_need_update;

/*
 * Note about RCU :
 * It is used to to delay the free of multiple probes array until a quiescent
 * state is reached.
 * Tracepoint entries modifications are protected by the tracepoint mutex.
 */
struct tracepoint_entry {
	struct cds_hlist_node hlist;
	struct lttng_ust_tracepoint_probe *probes;
	int refcount;	/* Number of times armed. 0 if disarmed. */
	int callsite_refcount;	/* how many libs use this tracepoint */
	char *signature;
	char *name;
};

struct tp_probes {
	union {
		struct cds_list_head list;
		/* Field below only used for call_rcu scheme */
		/* struct rcu_head head; */
	} u;
	struct lttng_ust_tracepoint_probe probes[0];
};

/*
 * Callsite hash table, containing the tracepoint call sites.
 * Protected by tracepoint mutex.
 */
#define CALLSITE_HASH_BITS 12
#define CALLSITE_TABLE_SIZE (1 << CALLSITE_HASH_BITS)
static struct cds_hlist_head callsite_table[CALLSITE_TABLE_SIZE];

struct callsite_entry {
	struct cds_hlist_node hlist;	/* hash table node */
	struct cds_list_head node;	/* lib list of callsites node */
	struct lttng_ust_tracepoint *tp;
	bool tp_entry_callsite_ref; /* Has a tp_entry took a ref on this callsite */
};

static int tracepoint_v1_api_used;
static void (*lttng_ust_liburcu_bp_synchronize_rcu)(void);
static void (*lttng_ust_liburcu_bp_rcu_read_lock)(void);
static void (*lttng_ust_liburcu_bp_rcu_read_unlock)(void);
void (*lttng_ust_liburcu_bp_before_fork)(void);
void (*lttng_ust_liburcu_bp_after_fork_parent)(void);
void (*lttng_ust_liburcu_bp_after_fork_child)(void);

static bool lttng_ust_tracepoint_v1_used(void)
{
	return uatomic_read(&tracepoint_v1_api_used);
}

static void lttng_ust_tracepoint_set_v1_used(void)
{
	if (!lttng_ust_tracepoint_v1_used()) {
		/*
		 * Perform dlsym here rather than lazily on first use to
		 * eliminate nesting of dynamic loader lock (used within
		 * dlsym) inside the ust lock.
		 */
		if (!lttng_ust_liburcu_bp_synchronize_rcu) {
			lttng_ust_liburcu_bp_synchronize_rcu = URCU_FORCE_CAST(void (*)(void),
				dlsym(RTLD_DEFAULT, "synchronize_rcu_bp"));
			/*
			 * Allow legacy applications compiled without
			 * _LGPL_SOURCE to use v1 API. Those are not
			 * required to be linked against liburcu-bp,
			 * so in those situations the liburcu-bp symbols
			 * are not present in the global symbol table,
			 * and we do not need to call urcu-bp
			 * synchronize.
			 *
			 * However, nothing prevents a _LGPL_SOURCE
			 * instrumented library loaded afterwards to
			 * require liburcu-bp, so we need to check again
			 * in that situation.
			 */
			if (!lttng_ust_liburcu_bp_synchronize_rcu)
				return;
		}
		if (!lttng_ust_liburcu_bp_before_fork) {
			lttng_ust_liburcu_bp_before_fork = URCU_FORCE_CAST(void (*)(void),
				dlsym(RTLD_DEFAULT, "rcu_bp_before_fork"));
			if (!lttng_ust_liburcu_bp_before_fork)
				abort();
		}
		if (!lttng_ust_liburcu_bp_after_fork_parent) {
			lttng_ust_liburcu_bp_after_fork_parent = URCU_FORCE_CAST(void (*)(void),
				dlsym(RTLD_DEFAULT, "rcu_bp_after_fork_parent"));
			if (!lttng_ust_liburcu_bp_after_fork_parent)
				abort();
		}
		if (!lttng_ust_liburcu_bp_after_fork_child) {
			lttng_ust_liburcu_bp_after_fork_child = URCU_FORCE_CAST(void (*)(void),
				dlsym(RTLD_DEFAULT, "rcu_bp_after_fork_child"));
			if (!lttng_ust_liburcu_bp_after_fork_child)
				abort();
		}
		if (!lttng_ust_liburcu_bp_rcu_read_lock) {
			lttng_ust_liburcu_bp_rcu_read_lock = URCU_FORCE_CAST(void (*)(void),
				dlsym(RTLD_DEFAULT, "rcu_read_lock_bp"));
			if (!lttng_ust_liburcu_bp_rcu_read_lock)
				abort();
		}
		if (!lttng_ust_liburcu_bp_rcu_read_unlock) {
			lttng_ust_liburcu_bp_rcu_read_unlock = URCU_FORCE_CAST(void (*)(void),
				dlsym(RTLD_DEFAULT, "rcu_read_unlock_bp"));
			if (!lttng_ust_liburcu_bp_rcu_read_unlock)
				abort();
		}

		/* Fixup URCU bp TLS. */
		lttng_ust_liburcu_bp_rcu_read_lock();
		lttng_ust_liburcu_bp_rcu_read_unlock();

		uatomic_set(&tracepoint_v1_api_used, 1);
	}
}

/* coverity[+alloc] */
static void *allocate_probes(int count)
{
	struct tp_probes *p =
		zmalloc(count * sizeof(struct lttng_ust_tracepoint_probe)
		+ sizeof(struct tp_probes));
	return p == NULL ? NULL : p->probes;
}

/* coverity[+free : arg-0] */
static void release_probes(void *old)
{
	if (old) {
		struct tp_probes *tp_probes = caa_container_of(old,
			struct tp_probes, probes[0]);
		lttng_ust_synchronize_trace();
		free(tp_probes);
	}
}

static void debug_print_probes(struct tracepoint_entry *entry)
{
	int i;

	if (!tracepoint_debug || !entry->probes)
		return;

	for (i = 0; entry->probes[i].func; i++)
		DBG("Probe %d : %p", i, entry->probes[i].func);
}

static void *
tracepoint_entry_add_probe(struct tracepoint_entry *entry,
			   void (*probe)(void), void *data)
{
	int nr_probes = 0;
	struct lttng_ust_tracepoint_probe *old, *new;

	if (!probe) {
		WARN_ON(1);
		return ERR_PTR(-EINVAL);
	}
	debug_print_probes(entry);
	old = entry->probes;
	if (old) {
		/* (N -> N+1), (N != 0, 1) probes */
		for (nr_probes = 0; old[nr_probes].func; nr_probes++)
			if (old[nr_probes].func == probe &&
			    old[nr_probes].data == data)
				return ERR_PTR(-EEXIST);
	}
	/* + 2 : one for new probe, one for NULL func */
	new = allocate_probes(nr_probes + 2);
	if (new == NULL)
		return ERR_PTR(-ENOMEM);
	if (old)
		memcpy(new, old,
		       nr_probes * sizeof(struct lttng_ust_tracepoint_probe));
	new[nr_probes].func = probe;
	new[nr_probes].data = data;
	new[nr_probes + 1].func = NULL;
	entry->refcount = nr_probes + 1;
	entry->probes = new;
	debug_print_probes(entry);
	return old;
}

static void *
tracepoint_entry_remove_probe(struct tracepoint_entry *entry,
			      void (*probe)(void), void *data)
{
	int nr_probes = 0, nr_del = 0, i;
	struct lttng_ust_tracepoint_probe *old, *new;

	old = entry->probes;

	if (!old)
		return ERR_PTR(-ENOENT);

	debug_print_probes(entry);
	/* (N -> M), (N > 1, M >= 0) probes */
	if (probe) {
		for (nr_probes = 0; old[nr_probes].func; nr_probes++) {
			if (old[nr_probes].func == probe &&
			     old[nr_probes].data == data)
				nr_del++;
		}
	}

	if (nr_probes - nr_del == 0) {
		/* N -> 0, (N > 1) */
		entry->probes = NULL;
		entry->refcount = 0;
		debug_print_probes(entry);
		return old;
	} else {
		int j = 0;
		/* N -> M, (N > 1, M > 0) */
		/* + 1 for NULL */
		new = allocate_probes(nr_probes - nr_del + 1);
		if (new == NULL)
			return ERR_PTR(-ENOMEM);
		for (i = 0; old[i].func; i++)
			if (old[i].func != probe || old[i].data != data)
				new[j++] = old[i];
		new[nr_probes - nr_del].func = NULL;
		entry->refcount = nr_probes - nr_del;
		entry->probes = new;
	}
	debug_print_probes(entry);
	return old;
}

/*
 * Get tracepoint if the tracepoint is present in the tracepoint hash table.
 * Must be called with tracepoint mutex held.
 * Returns NULL if not present.
 */
static struct tracepoint_entry *get_tracepoint(const char *name)
{
	struct cds_hlist_head *head;
	struct cds_hlist_node *node;
	struct tracepoint_entry *e;
	size_t name_len = strlen(name);
	uint32_t hash;

	if (name_len > LTTNG_UST_SYM_NAME_LEN - 1) {
		WARN("Truncating tracepoint name %s which exceeds size limits of %u chars", name, LTTNG_UST_SYM_NAME_LEN - 1);
		name_len = LTTNG_UST_SYM_NAME_LEN - 1;
	}
	hash = jhash(name, name_len, 0);
	head = &tracepoint_table[hash & (TRACEPOINT_TABLE_SIZE - 1)];
	cds_hlist_for_each_entry(e, node, head, hlist) {
		if (!strncmp(name, e->name, LTTNG_UST_SYM_NAME_LEN - 1))
			return e;
	}
	return NULL;
}

/*
 * Add the tracepoint to the tracepoint hash table. Must be called with
 * tracepoint mutex held.
 */
static struct tracepoint_entry *add_tracepoint(const char *name,
		const char *signature)
{
	struct cds_hlist_head *head;
	struct cds_hlist_node *node;
	struct tracepoint_entry *e;
	size_t name_len = strlen(name);
	size_t sig_len = strlen(signature);
	size_t sig_off, name_off;
	uint32_t hash;

	if (name_len > LTTNG_UST_SYM_NAME_LEN - 1) {
		WARN("Truncating tracepoint name %s which exceeds size limits of %u chars", name, LTTNG_UST_SYM_NAME_LEN - 1);
		name_len = LTTNG_UST_SYM_NAME_LEN - 1;
	}
	hash = jhash(name, name_len, 0);
	head = &tracepoint_table[hash & (TRACEPOINT_TABLE_SIZE - 1)];
	cds_hlist_for_each_entry(e, node, head, hlist) {
		if (!strncmp(name, e->name, LTTNG_UST_SYM_NAME_LEN - 1)) {
			DBG("tracepoint %s busy", name);
			return ERR_PTR(-EEXIST);	/* Already there */
		}
	}

	/*
	 * Using zmalloc here to allocate a variable length elements: name and
	 * signature. Could cause some memory fragmentation if overused.
	 */
	name_off = sizeof(struct tracepoint_entry);
	sig_off = name_off + name_len + 1;

	e = zmalloc(sizeof(struct tracepoint_entry) + name_len + 1 + sig_len + 1);
	if (!e)
		return ERR_PTR(-ENOMEM);
	e->name = (char *) e + name_off;
	memcpy(e->name, name, name_len + 1);
	e->name[name_len] = '\0';

	e->signature = (char *) e + sig_off;
	memcpy(e->signature, signature, sig_len + 1);
	e->signature[sig_len] = '\0';

	e->probes = NULL;
	e->refcount = 0;
	e->callsite_refcount = 0;

	cds_hlist_add_head(&e->hlist, head);
	return e;
}

/*
 * Remove the tracepoint from the tracepoint hash table. Must be called with
 * tracepoint mutex held.
 */
static void remove_tracepoint(struct tracepoint_entry *e)
{
	cds_hlist_del(&e->hlist);
	free(e);
}

/*
 * Sets the probe callback corresponding to one tracepoint.
 */
static void set_tracepoint(struct tracepoint_entry **entry,
	struct lttng_ust_tracepoint *elem, int active)
{
	WARN_ON(strncmp((*entry)->name, elem->name, LTTNG_UST_SYM_NAME_LEN - 1) != 0);
	/*
	 * Check that signatures match before connecting a probe to a
	 * tracepoint. Warn the user if they don't.
	 */
	if (strcmp(elem->signature, (*entry)->signature) != 0) {
		static int warned = 0;

		/* Only print once, don't flood console. */
		if (!warned) {
			WARN("Tracepoint signature mismatch, not enabling one or more tracepoints. Ensure that the tracepoint probes prototypes match the application.");
			WARN("Tracepoint \"%s\" signatures: call: \"%s\" vs probe: \"%s\".",
				elem->name, elem->signature, (*entry)->signature);
			warned = 1;
		}
		/* Don't accept connecting non-matching signatures. */
		return;
	}

	/*
	 * rcu_assign_pointer has a cmm_smp_wmb() which makes sure that the new
	 * probe callbacks array is consistent before setting a pointer to it.
	 * This array is referenced by __DO_TRACE from
	 * include/linux/tracepoints.h. A matching cmm_smp_read_barrier_depends()
	 * is used.
	 */
	lttng_ust_rcu_assign_pointer(elem->probes, (*entry)->probes);
	CMM_STORE_SHARED(elem->state, active);
}

/*
 * Disable a tracepoint and its probe callback.
 * Note: only waiting an RCU period after setting elem->call to the empty
 * function insures that the original callback is not used anymore. This insured
 * by preempt_disable around the call site.
 */
static void disable_tracepoint(struct lttng_ust_tracepoint *elem)
{
	CMM_STORE_SHARED(elem->state, 0);
	lttng_ust_rcu_assign_pointer(elem->probes, NULL);
}

/*
 * Add the callsite to the callsite hash table. Must be called with
 * tracepoint mutex held.
 */
static void add_callsite(struct tracepoint_lib * lib, struct lttng_ust_tracepoint *tp)
{
	struct cds_hlist_head *head;
	struct callsite_entry *e;
	const char *name = tp->name;
	size_t name_len = strlen(name);
	uint32_t hash;
	struct tracepoint_entry *tp_entry;

	if (name_len > LTTNG_UST_SYM_NAME_LEN - 1) {
		WARN("Truncating tracepoint name %s which exceeds size limits of %u chars", name, LTTNG_UST_SYM_NAME_LEN - 1);
		name_len = LTTNG_UST_SYM_NAME_LEN - 1;
	}
	hash = jhash(name, name_len, 0);
	head = &callsite_table[hash & (CALLSITE_TABLE_SIZE - 1)];
	e = zmalloc(sizeof(struct callsite_entry));
	if (!e) {
		PERROR("Unable to add callsite for tracepoint \"%s\"", name);
		return;
	}
	cds_hlist_add_head(&e->hlist, head);
	e->tp = tp;
	cds_list_add(&e->node, &lib->callsites);

	tp_entry = get_tracepoint(name);
	if (!tp_entry)
		return;
	tp_entry->callsite_refcount++;
	e->tp_entry_callsite_ref = true;
}

/*
 * Remove the callsite from the callsite hash table and from lib
 * callsite list. Must be called with tracepoint mutex held.
 */
static void remove_callsite(struct callsite_entry *e)
{
	struct tracepoint_entry *tp_entry;

	tp_entry = get_tracepoint(e->tp->name);
	if (tp_entry) {
		if (e->tp_entry_callsite_ref)
			tp_entry->callsite_refcount--;
		if (tp_entry->callsite_refcount == 0)
			disable_tracepoint(e->tp);
	}
	cds_hlist_del(&e->hlist);
	cds_list_del(&e->node);
	free(e);
}

/*
 * Enable/disable all callsites based on the state of a specific
 * tracepoint entry.
 * Must be called with tracepoint mutex held.
 */
static void tracepoint_sync_callsites(const char *name)
{
	struct cds_hlist_head *head;
	struct cds_hlist_node *node;
	struct callsite_entry *e;
	size_t name_len = strlen(name);
	uint32_t hash;
	struct tracepoint_entry *tp_entry;

	tp_entry = get_tracepoint(name);
	if (name_len > LTTNG_UST_SYM_NAME_LEN - 1) {
		WARN("Truncating tracepoint name %s which exceeds size limits of %u chars", name, LTTNG_UST_SYM_NAME_LEN - 1);
		name_len = LTTNG_UST_SYM_NAME_LEN - 1;
	}
	hash = jhash(name, name_len, 0);
	head = &callsite_table[hash & (CALLSITE_TABLE_SIZE - 1)];
	cds_hlist_for_each_entry(e, node, head, hlist) {
		struct lttng_ust_tracepoint *tp = e->tp;

		if (strncmp(name, tp->name, LTTNG_UST_SYM_NAME_LEN - 1))
			continue;
		if (tp_entry) {
			if (!e->tp_entry_callsite_ref) {
				tp_entry->callsite_refcount++;
				e->tp_entry_callsite_ref = true;
			}
			set_tracepoint(&tp_entry, tp,
					!!tp_entry->refcount);
		} else {
			disable_tracepoint(tp);
			e->tp_entry_callsite_ref = false;
		}
	}
}

/**
 * tracepoint_update_probe_range - Update a probe range
 * @begin: beginning of the range
 * @end: end of the range
 *
 * Updates the probe callback corresponding to a range of tracepoints.
 */
static
void tracepoint_update_probe_range(struct lttng_ust_tracepoint * const *begin,
				   struct lttng_ust_tracepoint * const *end)
{
	struct lttng_ust_tracepoint * const *iter;
	struct tracepoint_entry *mark_entry;

	for (iter = begin; iter < end; iter++) {
		if (!*iter)
			continue;	/* skip dummy */
		if (!(*iter)->name) {
			disable_tracepoint(*iter);
			continue;
		}
		mark_entry = get_tracepoint((*iter)->name);
		if (mark_entry) {
			set_tracepoint(&mark_entry, *iter,
					!!mark_entry->refcount);
		} else {
			disable_tracepoint(*iter);
		}
	}
}

static void lib_update_tracepoints(struct tracepoint_lib *lib)
{
	tracepoint_update_probe_range(lib->tracepoints_start,
			lib->tracepoints_start + lib->tracepoints_count);
}

static void lib_register_callsites(struct tracepoint_lib *lib)
{
	struct lttng_ust_tracepoint * const *begin;
	struct lttng_ust_tracepoint * const *end;
	struct lttng_ust_tracepoint * const *iter;

	begin = lib->tracepoints_start;
	end = lib->tracepoints_start + lib->tracepoints_count;

	for (iter = begin; iter < end; iter++) {
		if (!*iter)
			continue;	/* skip dummy */
		if (!(*iter)->name) {
			continue;
		}
		add_callsite(lib, *iter);
	}
}

static void lib_unregister_callsites(struct tracepoint_lib *lib)
{
	struct callsite_entry *callsite, *tmp;

	cds_list_for_each_entry_safe(callsite, tmp, &lib->callsites, node)
		remove_callsite(callsite);
}

/*
 * Update probes, removing the faulty probes.
 */
static void tracepoint_update_probes(void)
{
	struct tracepoint_lib *lib;

	/* tracepoints registered from libraries and executable. */
	cds_list_for_each_entry(lib, &libs, list)
		lib_update_tracepoints(lib);
}

static struct lttng_ust_tracepoint_probe *
tracepoint_add_probe(const char *name, void (*probe)(void), void *data,
		const char *signature)
{
	struct tracepoint_entry *entry;
	struct lttng_ust_tracepoint_probe *old;

	entry = get_tracepoint(name);
	if (entry) {
		if (strcmp(entry->signature, signature) != 0) {
			ERR("Tracepoint and probe signature do not match.");
			return ERR_PTR(-EINVAL);
		}
	} else {
		entry = add_tracepoint(name, signature);
		if (IS_ERR(entry))
			return (struct lttng_ust_tracepoint_probe *)entry;
	}
	old = tracepoint_entry_add_probe(entry, probe, data);
	if (IS_ERR(old) && !entry->refcount)
		remove_tracepoint(entry);
	return old;
}

static void tracepoint_release_queue_add_old_probes(void *old)
{
	release_queue_need_update = 1;
	if (old) {
		struct tp_probes *tp_probes = caa_container_of(old,
			struct tp_probes, probes[0]);
		cds_list_add(&tp_probes->u.list, &release_queue);
	}
}

/**
 * __tracepoint_probe_register -  Connect a probe to a tracepoint
 * @name: tracepoint name
 * @probe: probe handler
 *
 * Returns 0 if ok, error value on error.
 * The probe address must at least be aligned on the architecture pointer size.
 * Called with the tracepoint mutex held.
 */
int __tracepoint_probe_register(const char *name, void (*probe)(void),
		void *data, const char *signature)
{
	void *old;
	int ret = 0;

	DBG("Registering probe to tracepoint %s", name);

	pthread_mutex_lock(&tracepoint_mutex);
	old = tracepoint_add_probe(name, probe, data, signature);
	if (IS_ERR(old)) {
		ret = PTR_ERR(old);
		goto end;
	}

	tracepoint_sync_callsites(name);
	release_probes(old);
end:
	pthread_mutex_unlock(&tracepoint_mutex);
	return ret;
}

/*
 * Caller needs to invoke __tracepoint_probe_release_queue() after
 * calling __tracepoint_probe_register_queue_release() one or multiple
 * times to ensure it does not leak memory.
 */
int __tracepoint_probe_register_queue_release(const char *name,
		void (*probe)(void), void *data, const char *signature)
{
	void *old;
	int ret = 0;

	DBG("Registering probe to tracepoint %s. Queuing release.", name);

	pthread_mutex_lock(&tracepoint_mutex);
	old = tracepoint_add_probe(name, probe, data, signature);
	if (IS_ERR(old)) {
		ret = PTR_ERR(old);
		goto end;
	}

	tracepoint_sync_callsites(name);
	tracepoint_release_queue_add_old_probes(old);
end:
	pthread_mutex_unlock(&tracepoint_mutex);
	return ret;
}

static void *tracepoint_remove_probe(const char *name, void (*probe)(void),
		void *data)
{
	struct tracepoint_entry *entry;
	void *old;

	entry = get_tracepoint(name);
	if (!entry)
		return ERR_PTR(-ENOENT);
	old = tracepoint_entry_remove_probe(entry, probe, data);
	if (IS_ERR(old))
		return old;
	if (!entry->refcount)
		remove_tracepoint(entry);
	return old;
}

/**
 * tracepoint_probe_unregister -  Disconnect a probe from a tracepoint
 * @name: tracepoint name
 * @probe: probe function pointer
 * @probe: probe data pointer
 */
int __tracepoint_probe_unregister(const char *name, void (*probe)(void),
		void *data)
{
	void *old;
	int ret = 0;

	DBG("Un-registering probe from tracepoint %s", name);

	pthread_mutex_lock(&tracepoint_mutex);
	old = tracepoint_remove_probe(name, probe, data);
	if (IS_ERR(old)) {
		ret = PTR_ERR(old);
		goto end;
	}
	tracepoint_sync_callsites(name);
	release_probes(old);
end:
	pthread_mutex_unlock(&tracepoint_mutex);
	return ret;
}

/*
 * Caller needs to invoke __tracepoint_probe_release_queue() after
 * calling __tracepoint_probe_unregister_queue_release() one or multiple
 * times to ensure it does not leak memory.
 */
int __tracepoint_probe_unregister_queue_release(const char *name,
		void (*probe)(void), void *data)
{
	void *old;
	int ret = 0;

	DBG("Un-registering probe from tracepoint %s. Queuing release.", name);

	pthread_mutex_lock(&tracepoint_mutex);
	old = tracepoint_remove_probe(name, probe, data);
	if (IS_ERR(old)) {
		ret = PTR_ERR(old);
		goto end;
	}
	tracepoint_sync_callsites(name);
	tracepoint_release_queue_add_old_probes(old);
end:
	pthread_mutex_unlock(&tracepoint_mutex);
	return ret;
}

void __tracepoint_probe_prune_release_queue(void)
{
	CDS_LIST_HEAD(release_probes);
	struct tp_probes *pos, *next;

	DBG("Release queue of unregistered tracepoint probes.");

	pthread_mutex_lock(&tracepoint_mutex);
	if (!release_queue_need_update)
		goto end;
	if (!cds_list_empty(&release_queue))
		cds_list_replace_init(&release_queue, &release_probes);
	release_queue_need_update = 0;

	/* Wait for grace period between all sync_callsites and free. */
	lttng_ust_synchronize_trace();

	cds_list_for_each_entry_safe(pos, next, &release_probes, u.list) {
		cds_list_del(&pos->u.list);
		free(pos);
	}
end:
	pthread_mutex_unlock(&tracepoint_mutex);
}

static void tracepoint_add_old_probes(void *old)
{
	need_update = 1;
	if (old) {
		struct tp_probes *tp_probes = caa_container_of(old,
			struct tp_probes, probes[0]);
		cds_list_add(&tp_probes->u.list, &old_probes);
	}
}

/**
 * tracepoint_probe_register_noupdate -  register a probe but not connect
 * @name: tracepoint name
 * @probe: probe handler
 *
 * caller must call tracepoint_probe_update_all()
 */
int tracepoint_probe_register_noupdate(const char *name, void (*probe)(void),
				       void *data, const char *signature)
{
	void *old;
	int ret = 0;

	pthread_mutex_lock(&tracepoint_mutex);
	old = tracepoint_add_probe(name, probe, data, signature);
	if (IS_ERR(old)) {
		ret = PTR_ERR(old);
		goto end;
	}
	tracepoint_add_old_probes(old);
end:
	pthread_mutex_unlock(&tracepoint_mutex);
	return ret;
}

/**
 * tracepoint_probe_unregister_noupdate -  remove a probe but not disconnect
 * @name: tracepoint name
 * @probe: probe function pointer
 *
 * caller must call tracepoint_probe_update_all()
 * Called with the tracepoint mutex held.
 */
int tracepoint_probe_unregister_noupdate(const char *name, void (*probe)(void),
					 void *data)
{
	void *old;
	int ret = 0;

	DBG("Un-registering probe from tracepoint %s", name);

	pthread_mutex_lock(&tracepoint_mutex);
	old = tracepoint_remove_probe(name, probe, data);
	if (IS_ERR(old)) {
		ret = PTR_ERR(old);
		goto end;
	}
	tracepoint_add_old_probes(old);
end:
	pthread_mutex_unlock(&tracepoint_mutex);
	return ret;
}

/**
 * tracepoint_probe_update_all -  update tracepoints
 */
void tracepoint_probe_update_all(void)
{
	CDS_LIST_HEAD(release_probes);
	struct tp_probes *pos, *next;

	pthread_mutex_lock(&tracepoint_mutex);
	if (!need_update) {
		goto end;
	}
	if (!cds_list_empty(&old_probes))
		cds_list_replace_init(&old_probes, &release_probes);
	need_update = 0;

	tracepoint_update_probes();
	/* Wait for grace period between update_probes and free. */
	lttng_ust_synchronize_trace();
	cds_list_for_each_entry_safe(pos, next, &release_probes, u.list) {
		cds_list_del(&pos->u.list);
		free(pos);
	}
end:
	pthread_mutex_unlock(&tracepoint_mutex);
}

void tracepoint_set_new_tracepoint_cb(void (*cb)(struct lttng_ust_tracepoint *))
{
	new_tracepoint_cb = cb;
}

static void new_tracepoints(struct lttng_ust_tracepoint * const *start,
			    struct lttng_ust_tracepoint * const *end)
{
	if (new_tracepoint_cb) {
		struct lttng_ust_tracepoint * const *t;

		for (t = start; t < end; t++) {
			if (*t)
				new_tracepoint_cb(*t);
		}
	}
}

/*
 * tracepoint_{un,}register_lib is meant to be looked up by instrumented
 * applications through dlsym(). If found, those can register their
 * tracepoints, else those tracepoints will not be available for
 * tracing. The number at the end of those symbols acts as a major
 * version for tracepoints.
 *
 * Older instrumented applications should still work with newer
 * liblttng-ust, but it is fine that instrumented applications compiled
 * against recent liblttng-ust headers require a recent liblttng-ust
 * runtime for those tracepoints to be taken into account.
 */
int tracepoint_register_lib2(struct lttng_ust_tracepoint * const *tracepoints_start,
			     int tracepoints_count)
{
	struct tracepoint_lib *pl, *iter;

	init_tracepoint();

	pl = (struct tracepoint_lib *) zmalloc(sizeof(struct tracepoint_lib));
	if (!pl) {
		PERROR("Unable to register tracepoint lib");
		return -1;
	}
	pl->tracepoints_start = tracepoints_start;
	pl->tracepoints_count = tracepoints_count;
	CDS_INIT_LIST_HEAD(&pl->callsites);

	pthread_mutex_lock(&tracepoint_mutex);
	/*
	 * We sort the libs by struct lib pointer address.
	 */
	cds_list_for_each_entry_reverse(iter, &libs, list) {
		BUG_ON(iter == pl);    /* Should never be in the list twice */
		if (iter < pl) {
			/* We belong to the location right after iter. */
			cds_list_add(&pl->list, &iter->list);
			goto lib_added;
		}
	}
	/* We should be added at the head of the list */
	cds_list_add(&pl->list, &libs);
lib_added:
	new_tracepoints(tracepoints_start, tracepoints_start + tracepoints_count);
	lib_register_callsites(pl);
	lib_update_tracepoints(pl);
	pthread_mutex_unlock(&tracepoint_mutex);

	DBG("just registered a tracepoints section from %p and having %d tracepoints",
		tracepoints_start, tracepoints_count);
	if (ust_debug()) {
		int i;

		for (i = 0; i < tracepoints_count; i++) {
			DBG("registered tracepoint: %s", tracepoints_start[i]->name);
		}
	}

	return 0;
}

/* Exposed for backward compatibility with old instrumented applications. */
int tracepoint_register_lib(struct lttng_ust_tracepoint * const *tracepoints_start,
			     int tracepoints_count)
{
	lttng_ust_tracepoint_set_v1_used();
	return tracepoint_register_lib2(tracepoints_start, tracepoints_count);
}

int tracepoint_unregister_lib2(struct lttng_ust_tracepoint * const *tracepoints_start)
{
	struct tracepoint_lib *lib;

	pthread_mutex_lock(&tracepoint_mutex);
	cds_list_for_each_entry(lib, &libs, list) {
		if (lib->tracepoints_start != tracepoints_start)
			continue;

		cds_list_del(&lib->list);
		/*
		 * Unregistering a callsite also decreases the
		 * callsite reference count of the corresponding
		 * tracepoint, and disables the tracepoint if
		 * the reference count drops to zero.
		 */
		lib_unregister_callsites(lib);
		DBG("just unregistered a tracepoints section from %p",
			lib->tracepoints_start);
		free(lib);
		break;
	}
	pthread_mutex_unlock(&tracepoint_mutex);
	return 0;
}

/* Exposed for backward compatibility with old instrumented applications. */
int tracepoint_unregister_lib(struct lttng_ust_tracepoint * const *tracepoints_start)
{
	lttng_ust_tracepoint_set_v1_used();
	return tracepoint_unregister_lib2(tracepoints_start);
}

/*
 * Report in debug message whether the compiler correctly supports weak
 * hidden symbols. This test checks that the address associated with two
 * weak symbols with hidden visibility is the same when declared within
 * two compile units part of the same module.
 */
static void check_weak_hidden(void)
{
	DBG("Your compiler treats weak symbols with hidden visibility for integer objects as %s between compile units part of the same module.",
		&__tracepoint_test_symbol1 == lttng_ust_tp_check_weak_hidden1() ?
			"SAME address" :
			"DIFFERENT addresses");
	DBG("Your compiler treats weak symbols with hidden visibility for pointer objects as %s between compile units part of the same module.",
		&__tracepoint_test_symbol2 == lttng_ust_tp_check_weak_hidden2() ?
			"SAME address" :
			"DIFFERENT addresses");
	DBG("Your compiler treats weak symbols with hidden visibility for 24-byte structure objects as %s between compile units part of the same module.",
		&__tracepoint_test_symbol3 == lttng_ust_tp_check_weak_hidden3() ?
			"SAME address" :
			"DIFFERENT addresses");
}

void init_tracepoint(void)
{
	if (uatomic_xchg(&initialized, 1) == 1)
		return;
	init_usterr();
	check_weak_hidden();
}

void exit_tracepoint(void)
{
	initialized = 0;
}

/*
 * Create the wrapper symbols.
 */
#undef tp_rcu_read_lock
#undef tp_rcu_read_unlock
#undef tp_rcu_dereference

void tp_rcu_read_lock(void)
{
	lttng_ust_urcu_read_lock();
}

void tp_rcu_read_unlock(void)
{
	lttng_ust_urcu_read_unlock();
}

void *tp_rcu_dereference_sym(void *p)
{
	return lttng_ust_rcu_dereference(p);
}

/*
 * Programs that have threads that survive after they exit, and therefore call
 * library destructors, should disable the tracepoint destructors by calling
 * tp_disable_destructors(). This will leak the tracepoint
 * instrumentation library shared object, leaving its teardown to the operating
 * system process teardown.
 *
 * To access and/or modify this value, users need to use a combination of
 * dlopen(3) and dlsym(3) to get an handle on the
 * tp_disable_destructors and tp_get_destructors_state symbols below.
 */
void tp_disable_destructors(void)
{
	uatomic_set(&tracepoint_destructors_state, 0);
}

/*
 * Returns 1 if the destructors are enabled and should be executed.
 * Returns 0 if the destructors are disabled.
 */
int tp_get_destructors_state(void)
{
	return uatomic_read(&tracepoint_destructors_state);
}

void lttng_ust_synchronize_trace(void)
{
	lttng_ust_urcu_synchronize_rcu();
	/*
	 * For legacy tracepoint instrumentation, also wait for urcu-bp
	 * grace period.
	 */
	if (lttng_ust_liburcu_bp_synchronize_rcu)
		lttng_ust_liburcu_bp_synchronize_rcu();
}

/*
 * Create the wrapper symbols for legacy v1 API.
 */
void tp_rcu_read_lock_bp(void)
{
	lttng_ust_urcu_read_lock();
}

void tp_rcu_read_unlock_bp(void)
{
	lttng_ust_urcu_read_unlock();
}

void *tp_rcu_dereference_sym_bp(void *p)
{
	return lttng_ust_rcu_dereference(p);
}
