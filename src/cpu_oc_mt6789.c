#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/tracepoint.h>
#include <trace/hooks/cpufreq.h>

#include "oc_profile.h"

#define CPU_BIG 6
#define CPU_LITTLE 0
#define LUT_MAX_ENTRIES 32U
#define LUT_FREQ_MASK 0xfffU
#define LUT_ROW_SIZE 0x4U
#define REG_FREQ_LUT_TABLE 0
#define LUT_MAP_SIZE 0x200U
#define CPUFREQ_HW_STATUS BIT(0)

struct cluster_patch {
	int cpu;
	unsigned int stock_max;
	unsigned int oc;
	unsigned long lut_phys;
	struct cpufreq_frequency_table *tbl;
	struct cpufreq_frequency_table bak[LUT_MAX_ENTRIES + 1];
	unsigned int n;
	s32 qos_old;
	unsigned int max_old;
	unsigned int cpuinfo_max_old;
	u32 lut_raw_old;
	void __iomem *lut_map;
	bool table_patched;
	bool qos_patched;
	bool cpuinfo_patched;
	bool lut_patched;
	bool patching;
	bool done;
};

static unsigned int big_oc = PROFILE_BIG_OC;
module_param(big_oc, uint, 0644);
MODULE_PARM_DESC(big_oc, "big cluster OC frequency in kHz, 0 = skip");

static unsigned int little_oc = PROFILE_LITTLE_OC;
module_param(little_oc, uint, 0644);
MODULE_PARM_DESC(little_oc, "little cluster OC frequency in kHz, 0 = skip");

static bool patch_lut = PROFILE_PATCH_LUT;
module_param(patch_lut, bool, 0644);
MODULE_PARM_DESC(patch_lut, "patch MTK hardware LUT row 0 (see oc_profile.h)");

static unsigned long lut_phys_little = PROFILE_LUT_PHYS_LITTLE;
module_param(lut_phys_little, ulong, 0644);
MODULE_PARM_DESC(lut_phys_little, "physical base of little-cluster cpufreq-hw registers (0 = skip LUT)");

static unsigned long lut_phys_big = PROFILE_LUT_PHYS_BIG;
module_param(lut_phys_big, ulong, 0644);
MODULE_PARM_DESC(lut_phys_big, "physical base of big-cluster cpufreq-hw registers (0 = skip LUT)");

static bool hook_override = PROFILE_HOOK_OVERRIDE;
module_param(hook_override, bool, 0644);
MODULE_PARM_DESC(hook_override, "surgical resolve-hook override for the stock-max gap (see oc_profile.h)");

static bool ignore_verify = PROFILE_IGNORE_VERIFY;
module_param(ignore_verify, bool, 0644);
MODULE_PARM_DESC(ignore_verify, "keep patches loaded when refresh verify fails (default 0)");

static unsigned long touch_max_ptr;
module_param(touch_max_ptr, ulong, 0644);
MODULE_PARM_DESC(touch_max_ptr, "kallsyms address of touch_boost freq_max_request pointer (0 = skip)");

static unsigned long powerhal_max_ptr;
module_param(powerhal_max_ptr, ulong, 0644);
MODULE_PARM_DESC(powerhal_max_ptr, "kallsyms address of powerhal_cpu_ctrl freq_max_request pointer (0 = skip)");

static unsigned long fpsgo_max_ptr;
module_param(fpsgo_max_ptr, ulong, 0644);
MODULE_PARM_DESC(fpsgo_max_ptr, "kallsyms address of mtk_fpsgo_v3 fbt_max_freq pointer (0 = skip)");

static unsigned long fpsgo_cur_ptr;
module_param(fpsgo_cur_ptr, ulong, 0644);
MODULE_PARM_DESC(fpsgo_cur_ptr, "kallsyms address of mtk_fpsgo_v3 fbt_cur_ceiling pointer (0 = skip)");

static unsigned int step_guard = PROFILE_STEP_GUARD;
module_param(step_guard, uint, 0644);
MODULE_PARM_DESC(step_guard, "max OC delta above stock in kHz (default 200000)");

static unsigned long fpsgo_rq_ptr;
module_param(fpsgo_rq_ptr, ulong, 0644);
MODULE_PARM_DESC(fpsgo_rq_ptr, "kallsyms address of mtk_fpsgo_v3 fbt_cpu_rq pointer (0 = skip)");

static bool drop_powerhal = PROFILE_DROP_POWERHAL;
module_param(drop_powerhal, bool, 0644);
MODULE_PARM_DESC(drop_powerhal, "remove powerhal_cpu_ctrl max QoS votes (daemon keeps stomping them)");

#define VENDOR_REQ_SIZE 0x40U

struct vendor_mod {
	const char *name;
	unsigned long ptr_addr;
	struct freq_qos_request *req[2];
	struct freq_constraints *qos[2];
	s32 old[2];
	bool bumped[2];
	bool dropped[2];
};

static struct vendor_mod vendors[3];

static struct cluster_patch cp[2];
static bool hook_registered;
static atomic_t hook_count = ATOMIC_INIT(0);

static unsigned int table_len(struct cpufreq_frequency_table *table)
{
	unsigned int i;

	if (!table)
		return 0;
	for (i = 0; i <= LUT_MAX_ENTRIES; i++) {
		if (table[i].frequency == CPUFREQ_TABLE_END)
			return i;
	}
	return 0;
}

static int lock_policy(struct cluster_patch *c, struct cpufreq_policy **out)
{
	struct cpufreq_policy *policy;

	policy = cpufreq_cpu_get(c->cpu);
	if (!policy) {
		pr_err("[cpu-oc] cpu%d: no policy\n", c->cpu);
		return -ENOENT;
	}
	down_write(&policy->rwsem);
	if (cpumask_empty(policy->cpus)) {
		up_write(&policy->rwsem);
		cpufreq_cpu_put(policy);
		pr_err("[cpu-oc] cpu%d: policy is inactive\n", c->cpu);
		return -ENOENT;
	}
	*out = policy;
	return 0;
}

static void unlock_policy(struct cpufreq_policy *policy)
{
	up_write(&policy->rwsem);
	cpufreq_cpu_put(policy);
}

static int refresh_and_verify(struct cluster_patch *c, unsigned int expected)
{
	struct cpufreq_policy *policy;
	int ret = 0;

	cpufreq_update_policy(c->cpu);
	policy = cpufreq_cpu_get(c->cpu);
	if (!policy)
		return -ENOENT;

	down_read(&policy->rwsem);
	if (cpumask_empty(policy->cpus)) {
		ret = -ENOENT;
	} else {
		s32 qos = READ_ONCE(policy->constraints.max_freq.target_value);
		if (policy->freq_table != c->tbl ||
		    policy->freq_table[0].frequency != expected ||
		    policy->cpuinfo.max_freq != expected ||
		    policy->max != expected || qos != (s32)expected) {
			pr_err("[cpu-oc] cpu%d: want %u table0=%u cpuinfo=%u max=%u qos=%d\n",
			       c->cpu, expected,
			       policy->freq_table ? policy->freq_table[0].frequency : 0,
			       policy->cpuinfo.max_freq, policy->max, qos);
			ret = -EINVAL;
		}
	}
	up_read(&policy->rwsem);
	cpufreq_cpu_put(policy);

	if (ret)
		pr_err("[cpu-oc] cpu%d: refresh verify failed for %u kHz\n",
		       c->cpu, expected);
	return ret;
}

static bool target_valid(struct cluster_patch *c)
{
	if (!c->oc || c->oc <= c->stock_max ||
	    c->oc > c->stock_max + step_guard || c->oc > PROFILE_ABS_CAP ||
	    c->oc % 1000 || (c->oc / 1000) > LUT_FREQ_MASK) {
		pr_err("[cpu-oc] cpu%d target %u out of (stock %u, +%u], abort\n",
		       c->cpu, c->oc, c->stock_max, step_guard);
		return false;
	}
	return true;
}

static int validate_policy(struct cluster_patch *c, struct cpufreq_policy *policy)
{
	struct cpufreq_frequency_table *t;
	unsigned int n;
	u32 raw;

	t = policy->freq_table;
	n = table_len(t);
	if (!t || n == 0 || n > LUT_MAX_ENTRIES ||
	    policy->freq_table_sorted != CPUFREQ_TABLE_SORTED_DESCENDING) {
		pr_err("[cpu-oc] cpu%d: invalid table %px entries=%u sorted=%u\n",
		       c->cpu, t, n, policy->freq_table_sorted);
		return -EINVAL;
	}
	if (t[0].frequency != c->stock_max || t[n].frequency != CPUFREQ_TABLE_END) {
		pr_err("[cpu-oc] cpu%d: unexpected table head/tail %u/%u\n",
		       c->cpu, t[0].frequency, t[n].frequency);
		return -EINVAL;
	}
	if (policy->cpuinfo.max_freq != c->stock_max || policy->max > c->stock_max ||
	    policy->min > policy->max) {
		pr_err("[cpu-oc] cpu%d: policy limits min=%u max=%u cpuinfo=%u are invalid (stock %u)\n",
		       c->cpu, policy->min, policy->max, policy->cpuinfo.max_freq,
		       c->stock_max);
		return -EINVAL;
	}
	if (!policy->max_freq_req || !freq_qos_request_active(policy->max_freq_req) ||
	    policy->max_freq_req->type != FREQ_QOS_MAX ||
	    policy->max_freq_req->qos != &policy->constraints) {
		pr_err("[cpu-oc] cpu%d: invalid max QoS request\n", c->cpu);
		return -EINVAL;
	}
	c->tbl = t;
	c->n = n;
	c->qos_old = policy->max_freq_req->pnode.prio;
	c->max_old = policy->max;
	c->cpuinfo_max_old = policy->cpuinfo.max_freq;
	if (!patch_lut)
		return 0;
	if (!c->lut_phys) {
		pr_err("[cpu-oc] cpu%d: patch_lut requested without lut_phys\n", c->cpu);
		return -EINVAL;
	}
	c->lut_map = ioremap(c->lut_phys, LUT_MAP_SIZE);
	if (!c->lut_map) {
		pr_err("[cpu-oc] cpu%d: ioremap %lx failed\n", c->cpu, c->lut_phys);
		return -ENOMEM;
	}
	raw = readl_relaxed(c->lut_map);
	if (FIELD_GET(LUT_FREQ_MASK, raw) * 1000 != c->stock_max) {
		pr_err("[cpu-oc] cpu%d: LUT0=%u does not match stock %u\n",
		       c->cpu, FIELD_GET(LUT_FREQ_MASK, raw) * 1000, c->stock_max);
		iounmap(c->lut_map);
		c->lut_map = NULL;
		return -EINVAL;
	}
	c->lut_raw_old = raw;
	return 0;
}

static void backup_table(struct cluster_patch *c)
{
	unsigned int i;

	for (i = 0; i <= c->n; i++)
		c->bak[i] = c->tbl[i];
}

static int retune_table(struct cluster_patch *c)
{
	unsigned int i, delta;

	if (c->n < 2)
		return -EINVAL;
	delta = c->oc - c->stock_max;
	for (i = 0; i + 1 < c->n; i++)
		c->tbl[i].frequency = c->tbl[i].frequency + delta;
	c->table_patched = true;
	pr_info("[cpu-oc] cpu%d: table retuned +%u head=%u anchor=%u entries=%u\n",
		c->cpu, delta, c->tbl[0].frequency,
		c->tbl[c->n - 1].frequency, c->n);
	return 0;
}

static int update_qos(struct cluster_patch *c, struct cpufreq_policy *policy,
		      unsigned int value)
{
	int ret;

	ret = freq_qos_update_request(policy->max_freq_req, (s32)value);
	if (ret < 0) {
		pr_err("[cpu-oc] cpu%d: QoS update to %u failed: %d\n",
		       c->cpu, value, ret);
		return ret;
	}
	c->qos_patched = true;
	return 0;
}

static struct freq_qos_request *vendor_req(struct vendor_mod *v,
					   unsigned int idx)
{
	void *array;
	struct freq_qos_request *req;

	if (idx > 1 || !v->ptr_addr)
		return NULL;
	array = READ_ONCE(*(void * const *)v->ptr_addr);
	if (!array) {
		pr_err("[cpu-oc] %s: null request array\n", v->name);
		return NULL;
	}
	req = (struct freq_qos_request *)((char *)array + idx * VENDOR_REQ_SIZE);
	if (!freq_qos_request_active(req) || req->type != FREQ_QOS_MAX) {
		pr_err("[cpu-oc] %s: invalid max request idx %u\n", v->name, idx);
		return NULL;
	}
	return req;
}

static int bump_vendor_max(struct vendor_mod *v, unsigned int idx,
			   unsigned int value)
{
	struct freq_qos_request *req;
	int ret;

	if (!value)
		return 0;
	req = vendor_req(v, idx);
	if (!req)
		return v->ptr_addr ? -EINVAL : 0;
	v->old[idx] = req->pnode.prio;
	ret = freq_qos_update_request(req, (s32)value);
	if (ret < 0) {
		pr_err("[cpu-oc] %s: bump idx %u to %u failed: %d\n",
		       v->name, idx, value, ret);
		return ret;
	}
	v->req[idx] = req;
	v->bumped[idx] = true;
	pr_info("[cpu-oc] %s: max request idx %u %d -> %u\n",
		v->name, idx, v->old[idx], value);
	return 0;
}

static void restore_vendor_max(struct vendor_mod *v)
{
	unsigned int i;
	int ret;

	for (i = 0; i < 2; i++) {
		if (v->dropped[i]) {
			if (!freq_qos_request_active(v->req[i]) && v->qos[i]) {
				ret = freq_qos_add_request(v->qos[i],
							   v->req[i],
							   FREQ_QOS_MAX,
							   v->old[i]);
				if (ret < 0)
					pr_err("[cpu-oc] %s: re-add idx %u failed: %d\n",
					       v->name, i, ret);
			}
			v->dropped[i] = false;
			v->bumped[i] = false;
			continue;
		}
		if (!v->bumped[i])
			continue;
		if (freq_qos_request_active(v->req[i]) &&
		    v->req[i]->type == FREQ_QOS_MAX)
			freq_qos_update_request(v->req[i], v->old[i]);
		v->bumped[i] = false;
	}
}

static int drop_vendor_max(struct vendor_mod *v, unsigned int idx)
{
	struct freq_qos_request *req;
	int ret;

	req = vendor_req(v, idx);
	if (!req)
		return v->ptr_addr ? -EINVAL : 0;
	v->qos[idx] = req->qos;
	v->old[idx] = req->pnode.prio;
	ret = freq_qos_remove_request(req);
	if (ret < 0) {
		pr_err("[cpu-oc] %s: drop idx %u failed: %d\n",
		       v->name, idx, ret);
		return ret;
	}
	v->req[idx] = req;
	v->dropped[idx] = true;
	pr_info("[cpu-oc] %s: max request idx %u dropped (was %d)\n",
		v->name, idx, v->old[idx]);
	return 0;
}

struct fpsgo_cache {
	u32 *max_arr;
	u32 *cur_arr;
	u32 max_old[2];
	u32 cur_old[2];
	bool done[2];
	bool patched;
};

static struct fpsgo_cache fcache;

static int patch_fpsgo_cache(unsigned int idx, unsigned int stock,
			     unsigned int oc)
{
	u32 *max_arr, *cur_arr;
	u32 max_old, cur_old;

	if (!oc || !fpsgo_max_ptr || !fpsgo_cur_ptr || idx > 1)
		return 0;
	max_arr = READ_ONCE(*(u32 * const *)fpsgo_max_ptr);
	cur_arr = READ_ONCE(*(u32 * const *)fpsgo_cur_ptr);
	if (!max_arr || !cur_arr) {
		pr_err("[cpu-oc] fpsgo: null cache array\n");
		return -EINVAL;
	}
	max_old = READ_ONCE(max_arr[idx]);
	cur_old = READ_ONCE(cur_arr[idx]);
	if (max_old != stock || cur_old != stock) {
		pr_err("[cpu-oc] fpsgo: unexpected cache idx %u max=%u cur=%u (stock %u)\n",
		       idx, max_old, cur_old, stock);
		return -EINVAL;
	}
	fcache.max_arr = max_arr;
	fcache.cur_arr = cur_arr;
	fcache.max_old[idx] = max_old;
	fcache.cur_old[idx] = cur_old;
	WRITE_ONCE(max_arr[idx], oc);
	WRITE_ONCE(cur_arr[idx], oc);
	if (READ_ONCE(max_arr[idx]) != oc || READ_ONCE(cur_arr[idx]) != oc) {
		pr_err("[cpu-oc] fpsgo: cache write/read mismatch idx %u\n", idx);
		WRITE_ONCE(max_arr[idx], max_old);
		WRITE_ONCE(cur_arr[idx], cur_old);
		return -EIO;
	}
	fcache.patched = true;
	fcache.done[idx] = true;
	pr_info("[cpu-oc] fpsgo: cache idx %u %u -> %u\n", idx, stock, oc);
	return 0;
}

static void restore_fpsgo_cache(void)
{
	unsigned int i;

	if (!fcache.patched)
		return;
	for (i = 0; i < 2; i++) {
		if (!fcache.done[i])
			continue;
		if (READ_ONCE(fcache.max_arr[i]) != fcache.max_old[i])
			WRITE_ONCE(fcache.max_arr[i], fcache.max_old[i]);
		if (READ_ONCE(fcache.cur_arr[i]) != fcache.cur_old[i])
			WRITE_ONCE(fcache.cur_arr[i], fcache.cur_old[i]);
		fcache.done[i] = false;
	}
	fcache.patched = false;
}

static int patch_lut_row(struct cluster_patch *c, unsigned int value)
{
	u32 old_raw, new_raw;

	if (!patch_lut)
		return 0;
	if (!c->lut_map) {
		pr_err("[cpu-oc] cpu%d: LUT not mapped\n", c->cpu);
		return -EINVAL;
	}
	old_raw = readl_relaxed(c->lut_map);
	if (FIELD_GET(LUT_FREQ_MASK, old_raw) * 1000 != c->stock_max) {
		pr_err("[cpu-oc] cpu%d: LUT changed before patch, abort\n", c->cpu);
		return -EINVAL;
	}
	new_raw = (old_raw & ~LUT_FREQ_MASK) | ((value / 1000) & LUT_FREQ_MASK);
	c->lut_patched = true;
	writel_relaxed(new_raw, c->lut_map);
	if (readl_relaxed(c->lut_map) != new_raw) {
		pr_err("[cpu-oc] cpu%d: LUT write/read mismatch\n", c->cpu);
		return -EIO;
	}
	pr_info("[cpu-oc] cpu%d: LUT0 %08x -> %08x\n", c->cpu, old_raw, new_raw);
	return 0;
}

static void restore_lut(struct cluster_patch *c)
{
	if (c->lut_patched && c->lut_map) {
		writel_relaxed(c->lut_raw_old, c->lut_map);
		if (readl_relaxed(c->lut_map) == c->lut_raw_old)
			c->lut_patched = false;
		else
			pr_err("[cpu-oc] cpu%d: LUT restore mismatch\n", c->cpu);
	}
}

static void unmap_lut(struct cluster_patch *c)
{
	if (c->lut_map) {
		iounmap(c->lut_map);
		c->lut_map = NULL;
	}
}

static int patch_one(struct cluster_patch *c)
{
	struct cpufreq_policy *policy;
	int ret, rollback_ret;

	if (!c->oc)
		return 0;
	if (!target_valid(c))
		return -EINVAL;
	ret = lock_policy(c, &policy);
	if (ret)
		return ret;
	ret = validate_policy(c, policy);
	if (ret)
		goto unlock;
	backup_table(c);
	ret = retune_table(c);
	if (ret)
		goto unlock;
	policy->cpuinfo.max_freq = c->oc;
	c->cpuinfo_patched = true;
	ret = update_qos(c, policy, c->oc);
	if (ret)
		goto rollback;
	WRITE_ONCE(c->patching, true);
	up_write(&policy->rwsem);

	ret = refresh_and_verify(c, c->oc);
	if (ret) {
		if (!ignore_verify) {
			cpufreq_cpu_put(policy);
			goto rollback_after_update;
		}
		pr_warn("[cpu-oc] cpu%d: verify failed, keeping patches (ignore_verify)\n",
			c->cpu);
		ret = 0;
	}
	ret = patch_lut_row(c, c->oc);
	if (ret) {
		cpufreq_cpu_put(policy);
		goto rollback_after_update;
	}
	WRITE_ONCE(c->patching, false);
	WRITE_ONCE(c->done, true);
	cpufreq_cpu_put(policy);
	pr_info("[cpu-oc] cpu%d: %u -> %u kHz, table=%u entries, LUT=%s\n",
		c->cpu, c->stock_max, c->oc, c->n, patch_lut ? "patched" : "unchanged");
	return 0;

rollback:
	if (c->cpuinfo_patched) {
		policy->cpuinfo.max_freq = c->cpuinfo_max_old;
		c->cpuinfo_patched = false;
	}
	if (c->table_patched) {
		unsigned int i;
		for (i = 0; i <= c->n; i++)
			c->tbl[i] = c->bak[i];
		c->table_patched = false;
	}
	WRITE_ONCE(c->patching, false);
	unmap_lut(c);
unlock:
	unlock_policy(policy);
	return ret;

rollback_after_update:
	WRITE_ONCE(c->patching, false);
	restore_lut(c);
	rollback_ret = lock_policy(c, &policy);
	if (rollback_ret) {
		pr_err("[cpu-oc] cpu%d: cannot lock policy during rollback\n", c->cpu);
		unmap_lut(c);
		return ret;
	}
	policy->max = c->max_old;
	if (c->qos_patched) {
		int qos_ret = update_qos(c, policy, (unsigned int)c->qos_old);
		if (qos_ret)
			pr_err("[cpu-oc] cpu%d: QoS rollback failed: %d\n",
			       c->cpu, qos_ret);
		c->qos_patched = false;
	}
	if (c->cpuinfo_patched) {
		policy->cpuinfo.max_freq = c->cpuinfo_max_old;
		c->cpuinfo_patched = false;
	}
	if (c->table_patched) {
		unsigned int i;
		for (i = 0; i <= c->n; i++)
			c->tbl[i] = c->bak[i];
		c->table_patched = false;
	}
	up_write(&policy->rwsem);
	cpufreq_update_policy(c->cpu);
	cpufreq_cpu_put(policy);
	restore_lut(c);
	unmap_lut(c);
	return ret;
}

static void restore_one(struct cluster_patch *c)
{
	struct cpufreq_policy *policy;
	int ret;

	if (!READ_ONCE(c->done)) {
		restore_lut(c);
		unmap_lut(c);
		return;
	}
	ret = lock_policy(c, &policy);
	if (ret) {
		restore_lut(c);
		unmap_lut(c);
		return;
	}
	if (policy->freq_table != c->tbl) {
		pr_err("[cpu-oc] cpu%d: table moved, memory restore skipped\n", c->cpu);
		unlock_policy(policy);
		restore_lut(c);
		unmap_lut(c);
		return;
	}
	policy->max = c->max_old;
	if (c->qos_patched) {
		ret = update_qos(c, policy, (unsigned int)c->qos_old);
		if (ret)
			pr_err("[cpu-oc] cpu%d: QoS restore failed: %d\n", c->cpu, ret);
		c->qos_patched = false;
	}
	if (c->cpuinfo_patched) {
		policy->cpuinfo.max_freq = c->cpuinfo_max_old;
		c->cpuinfo_patched = false;
	}
	if (c->table_patched) {
		unsigned int i;
		for (i = 0; i <= c->n; i++)
			c->tbl[i] = c->bak[i];
		c->table_patched = false;
	}
	up_write(&policy->rwsem);
	cpufreq_update_policy(c->cpu);
	cpufreq_cpu_put(policy);
	restore_lut(c);
	unmap_lut(c);
	WRITE_ONCE(c->done, false);
	pr_info("[cpu-oc] cpu%d restored to %u kHz\n", c->cpu, c->stock_max);
}

static void resolve_probe(void *unused, struct cpufreq_policy *policy,
			  unsigned int *target_freq,
			  unsigned int old_target_freq)
{
	struct cluster_patch *c;
	unsigned int clamped, count;
	int i;

	if (!READ_ONCE(hook_override) || !policy || !target_freq)
		return;
	clamped = READ_ONCE(*target_freq);
	for (i = 0; i < 2; i++) {
		c = &cp[i];
		if (!c->oc)
			continue;
		if (policy->cpu == c->cpu &&
		    (READ_ONCE(c->patching) || READ_ONCE(c->done)) &&
		    old_target_freq >= c->stock_max &&
		    clamped >= c->stock_max && clamped < c->oc) {
			WRITE_ONCE(*target_freq, c->oc);
			count = atomic_inc_return(&hook_count);
			if (count <= 8)
				pr_info("[cpu-oc] resolve cpu%d %u -> %u (old %u)\n",
					c->cpu, clamped, c->oc, old_target_freq);
			return;
		}
	}
}

static int register_hook(void)
{
	int ret;

	if (!hook_override)
		return 0;
	ret = register_trace_android_vh_cpufreq_resolve_freq(resolve_probe, NULL);
	if (ret) {
		pr_err("[cpu-oc]: cannot register resolve hook: %d\n", ret);
		return ret;
	}
	hook_registered = true;
	return 0;
}

static void unregister_hook(void)
{
	if (!hook_registered)
		return;
	unregister_trace_android_vh_cpufreq_resolve_freq(resolve_probe, NULL);
	tracepoint_synchronize_unregister();
	hook_registered = false;
}

static int __init cpu_oc_init(void)
{
	int ret, i;

	cp[0].cpu = CPU_BIG;
	cp[0].stock_max = PROFILE_STOCK_BIG_MAX;
	cp[0].oc = big_oc;
	cp[0].lut_phys = lut_phys_big;
	cp[1].cpu = CPU_LITTLE;
	cp[1].stock_max = PROFILE_STOCK_LITTLE_MAX;
	cp[1].oc = little_oc;
	cp[1].lut_phys = lut_phys_little;
	vendors[0].name = "touch_boost";
	vendors[0].ptr_addr = touch_max_ptr;
	vendors[1].name = "powerhal_cpu_ctrl";
	vendors[1].ptr_addr = powerhal_max_ptr;
	vendors[2].name = "mtk_fpsgo_v3";
	vendors[2].ptr_addr = fpsgo_rq_ptr;

	if ((big_oc && (big_oc <= PROFILE_STOCK_BIG_MAX || big_oc > PROFILE_STOCK_BIG_MAX + step_guard ||
			big_oc > PROFILE_ABS_CAP || big_oc % 1000 ||
			(big_oc / 1000) > LUT_FREQ_MASK)) ||
	    (little_oc && (little_oc <= PROFILE_STOCK_LITTLE_MAX ||
			   little_oc > PROFILE_STOCK_LITTLE_MAX + step_guard ||
			   little_oc > PROFILE_ABS_CAP || little_oc % 1000 ||
			   (little_oc / 1000) > LUT_FREQ_MASK))) {
		pr_err("[cpu-oc] target big=%u little=%u out of range, abort\n",
		       big_oc, little_oc);
		return -EINVAL;
	}

	ret = register_hook();
	if (ret)
		return ret;
	for (i = 0; i < 3; i++) {
		if (i == 1 && drop_powerhal) {
			ret = drop_vendor_max(&vendors[i], 1);
			if (ret)
				break;
			ret = drop_vendor_max(&vendors[i], 0);
			if (ret)
				break;
			continue;
		}
		ret = bump_vendor_max(&vendors[i], 1, big_oc);
		if (ret)
			break;
		ret = bump_vendor_max(&vendors[i], 0, little_oc);
		if (ret)
			break;
	}
	if (!ret)
		ret = patch_fpsgo_cache(1, PROFILE_STOCK_BIG_MAX, big_oc);
	if (!ret)
		ret = patch_fpsgo_cache(0, PROFILE_STOCK_LITTLE_MAX, little_oc);
	if (ret) {
		restore_fpsgo_cache();
		for (i = 0; i < 3; i++)
			restore_vendor_max(&vendors[i]);
		unregister_hook();
		return ret;
	}
	ret = patch_one(&cp[0]);
	if (ret) {
		restore_fpsgo_cache();
		for (i = 0; i < 3; i++)
			restore_vendor_max(&vendors[i]);
		unregister_hook();
		return ret;
	}
	ret = patch_one(&cp[1]);
	if (ret) {
		restore_fpsgo_cache();
		for (i = 0; i < 3; i++)
			restore_vendor_max(&vendors[i]);
		unregister_hook();
		restore_one(&cp[0]);
		return ret;
	}
	pr_info("[cpu-oc] loaded: big=%u little=%u LUT=%s hook_override=%s ignore_verify=%s\n",
		big_oc, little_oc, patch_lut ? "on" : "off",
		hook_override ? "on" : "off", ignore_verify ? "on" : "off");
	return 0;
}

static void __exit cpu_oc_exit(void)
{
	int i;

	unregister_hook();
	restore_fpsgo_cache();
	for (i = 0; i < 3; i++)
		restore_vendor_max(&vendors[i]);
	restore_one(&cp[0]);
	restore_one(&cp[1]);
	pr_info("[cpu-oc] unloaded\n");
}

module_init(cpu_oc_init);
module_exit(cpu_oc_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MT6789 CPU OC via policy, QoS, table, and hardware LUT patching");
