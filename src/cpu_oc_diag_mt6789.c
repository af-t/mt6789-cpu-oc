#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/cpufreq.h>
#include <linux/compiler.h>
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kprobes.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/pm_opp.h>
#include <linux/regulator/consumer.h>
#include <linux/tracepoint.h>
#include <trace/hooks/cpufreq.h>
#include <trace/hooks/power.h>

#define DIAG_MAX_ENTRIES 32U
#define DIAG_LUT_FREQ GENMASK(11, 0)
#define DIAG_LUT_ROW_SIZE 0x4
#define DIAG_REG_FREQ_LUT_TABLE 0
#define DIAG_REG_FREQ_ENABLE 1
#define DIAG_REG_FREQ_PERF_STATE 2
#define DIAG_REG_FREQ_HW_STATE 3
#define DIAG_REG_ARRAY_SIZE 6

struct diag_mtk_cpufreq_data {
	struct cpufreq_frequency_table *table;
	void __iomem *reg_bases[DIAG_REG_ARRAY_SIZE];
	struct resource *res;
	void __iomem *base;
	int nr_opp;
};

static bool trace_probe;
static bool trace_qos;
static bool trace_qosup;
static bool read_driver;
static bool read_mmio;
static bool read_opp = true;
static bool read_regulator = true;
static char *regulator_name = "cpu";
static bool trace_registered;
static bool trace_qos_registered;
static bool qosup_registered;
static unsigned long lut_phys;
static void __iomem *lut_map;
module_param(trace_probe, bool, 0644);
module_param(trace_qos, bool, 0644);
module_param(trace_qosup, bool, 0644);
module_param(read_driver, bool, 0644);
module_param(read_mmio, bool, 0644);
module_param(read_opp, bool, 0644);
module_param(read_regulator, bool, 0644);
module_param(regulator_name, charp, 0444);
module_param(lut_phys, ulong, 0644);
MODULE_PARM_DESC(trace_probe, "attach read-only resolve-frequency vendor-hook probe");
MODULE_PARM_DESC(trace_qos, "attach read-only freq-qos-update probe");
MODULE_PARM_DESC(trace_qosup, "kprobe freq_qos_update_request callers (read-only)");
MODULE_PARM_DESC(read_driver, "dereference policy driver_data for validation");
MODULE_PARM_DESC(read_mmio, "read MTK cpufreq MMIO registers after driver validation");
MODULE_PARM_DESC(read_opp, "read CPU OPP frequencies and target voltages");
MODULE_PARM_DESC(read_regulator, "read live CPU regulator voltage");
MODULE_PARM_DESC(regulator_name, "CPU regulator supply name (default cpu)");
MODULE_PARM_DESC(lut_phys, "physical base of MTK cpufreq-hw registers from device tree (0 = skip)");

#define LUT_MAP_SIZE 0x200U

static void diag_lut_phys(void)
{
	unsigned int i;
	u32 raw, freq, prev = 0;

	if (!lut_phys)
		return;
	lut_map = ioremap(lut_phys, LUT_MAP_SIZE);
	if (!lut_map) {
		pr_err("[cpu-oc-diag] ioremap %lx failed\n", lut_phys);
		return;
	}
	for (i = 0; i < DIAG_MAX_ENTRIES; i++) {
		raw = readl_relaxed(lut_map + i * DIAG_LUT_ROW_SIZE);
		freq = FIELD_GET(DIAG_LUT_FREQ, raw) * 1000;
		if (freq == prev)
			break;
		pr_info("[cpu-oc-diag] hwlut[%u]=%08x freq=%u\n", i, raw, freq);
		prev = freq;
	}
	pr_info("[cpu-oc-diag] hw enable=%08x perf_state=%08x hw_state=%08x\n",
		readl_relaxed(lut_map + 0x84),
		readl_relaxed(lut_map + 0x88),
		readl_relaxed(lut_map + 0x8c));
}

static atomic_t trace_count = ATOMIC_INIT(0);
static atomic_t qos_count = ATOMIC_INIT(0);
static atomic_t qosup_count = ATOMIC_INIT(0);
static struct kprobe qosup_kp;

static int qosup_pre(struct kprobe *kp, struct pt_regs *regs)
{
	struct freq_qos_request *req = (struct freq_qos_request *)regs->regs[0];
	s32 value = (s32)regs->regs[1];
	unsigned int count;

	count = atomic_inc_return(&qosup_count);
	if (count > 32 || !req)
		return 0;
	pr_info("[cpu-oc-diag] qos_update_call req=%px type=%u prio=%d value=%d comm=%s\n",
		req, READ_ONCE(req->type), READ_ONCE(req->pnode.prio), value,
		current->comm);
	dump_stack();
	return 0;
}

static void diag_qos_probe(void *unused, struct freq_qos_request *req, int value)
{
	unsigned int count;

	if (!req)
		return;
	count = atomic_inc_return(&qos_count);
	if (count > 32)
		return;
	pr_info("[cpu-oc-diag] qos_update req=%px type=%u prio=%d value=%d\n",
		req, READ_ONCE(req->type), READ_ONCE(req->pnode.prio), value);
}

static unsigned int diag_table_len(struct cpufreq_frequency_table *table)
{
	unsigned int i;

	if (!table)
		return 0;
	for (i = 0; i <= DIAG_MAX_ENTRIES; i++) {
		if (table[i].frequency == CPUFREQ_TABLE_END)
			return i;
	}
	return 0;
}

static void diag_opp(unsigned int cpu)
{
	struct device *dev;
	struct dev_pm_opp *opp;
	unsigned long freq, matched, volt, next;
	int count, i;

	if (!read_opp)
		return;
	dev = get_cpu_device(cpu);
	if (!dev) {
		pr_info("[cpu-oc-diag] cpu%u: CPU device unavailable\n", cpu);
		return;
	}
	count = dev_pm_opp_get_opp_count(dev);
	if (count < 0) {
		pr_info("[cpu-oc-diag] cpu%u: OPP table unavailable (%d)\n",
			cpu, count);
		return;
	}
	pr_info("[cpu-oc-diag] cpu%u: OPP entries=%d\n", cpu, count);
	freq = ULONG_MAX;
	for (i = 0; i < count; i++) {
		opp = dev_pm_opp_find_freq_floor(dev, &freq);
		if (IS_ERR(opp)) {
			pr_info("[cpu-oc-diag] cpu%u: OPP lookup failed (%ld)\n",
				cpu, PTR_ERR(opp));
			return;
		}
		matched = freq;
		volt = dev_pm_opp_get_voltage(opp);
		dev_pm_opp_put(opp);
		pr_info("[cpu-oc-diag] cpu%u: opp=%lu kHz vproc=%lu uV\n",
			cpu, matched / 1000, volt);
		if (!matched)
			break;
		next = matched - 1;
		if (next >= freq)
			break;
		freq = next;
	}
}

static void diag_regulator(unsigned int cpu)
{
	struct device *dev;
	struct regulator *reg;
	int volt, ret;

	if (!read_regulator)
		return;
	dev = get_cpu_device(cpu);
	if (!dev) {
		pr_info("[cpu-oc-diag] cpu%u: CPU device unavailable\n", cpu);
		return;
	}
	reg = regulator_get_optional(dev, regulator_name);
	if (IS_ERR(reg)) {
		ret = PTR_ERR(reg);
		pr_info("[cpu-oc-diag] cpu%u: regulator '%s' unavailable (%d)\n",
			cpu, regulator_name, ret);
		return;
	}
	volt = regulator_get_voltage(reg);
	regulator_put(reg);
	if (volt <= 0) {
		pr_info("[cpu-oc-diag] cpu%u: regulator '%s' reported no voltage (%d)\n",
			cpu, regulator_name, volt);
		return;
	}
	pr_info("[cpu-oc-diag] cpu%u: rail=%s vproc=%d uV\n", cpu, regulator_name, volt);
}

static void diag_policy(unsigned int cpu)
{
	struct cpufreq_policy *policy;
	struct diag_mtk_cpufreq_data *data;
	struct cpufreq_frequency_table *table;
	struct freq_qos_request *req;
	void __iomem *lut_base;
	unsigned int n, i;
	u32 raw, freq;

	policy = cpufreq_cpu_get(cpu);
	if (!policy) {
		pr_info("[cpu-oc-diag] cpu%u: no policy\n", cpu);
		return;
	}

	down_read(&policy->rwsem);
	if (cpumask_empty(policy->cpus)) {
		pr_info("[cpu-oc-diag] cpu%u: policy is inactive\n", cpu);
		goto out;
	}

	table = policy->freq_table;
	n = diag_table_len(table);
	req = policy->max_freq_req;
	data = policy->driver_data;

	pr_info("[cpu-oc-diag] policy%u ptr=%px cpu=%u cpus=%*pb min=%u max=%u cur=%u cpuinfo=%u-%u gov=%s table=%px entries=%u sorted=%u\n",
		cpu, policy, policy->cpu, cpumask_pr_args(policy->cpus),
		policy->min, policy->max, policy->cur,
		policy->cpuinfo.min_freq, policy->cpuinfo.max_freq,
		policy->governor ? policy->governor->name : "<none>",
		table, n, policy->freq_table_sorted);
	pr_info("[cpu-oc-diag] policy%u qos_max=%d max_req=%px active=%d type=%u prio=%d qos=%px\n",
		cpu, READ_ONCE(policy->constraints.max_freq.target_value), req,
		req ? freq_qos_request_active(req) : 0, req ? req->type : 0,
		req ? req->pnode.prio : 0, req ? req->qos : NULL);

	if (!read_driver) {
		pr_info("[cpu-oc-diag] policy%u: driver_data read disabled\n", cpu);
		for (i = 0; i < n; i++)
			pr_info("[cpu-oc-diag] policy%u table[%u]=%u\n",
				cpu, i, table[i].frequency);
		goto out;
	}
	if (!data) {
		pr_info("[cpu-oc-diag] policy%u: driver_data is NULL\n", cpu);
		goto out;
	}
	pr_info("[cpu-oc-diag] policy%u driver_data=%px table_match=%d nr_opp=%d base=%px res=%px\n",
		cpu, data, data->table == table, data->nr_opp, data->base, data->res);

	lut_base = data->reg_bases[DIAG_REG_FREQ_LUT_TABLE];
	if (data->table != table || data->nr_opp <= 0 ||
	    data->nr_opp > DIAG_MAX_ENTRIES || n != (unsigned int)data->nr_opp ||
	    !lut_base || !data->reg_bases[DIAG_REG_FREQ_ENABLE] ||
	    !data->reg_bases[DIAG_REG_FREQ_PERF_STATE] ||
	    !data->reg_bases[DIAG_REG_FREQ_HW_STATE]) {
		pr_info("[cpu-oc-diag] policy%u: refusing MMIO read, invalid driver_data\n", cpu);
		goto out;
	}

	if (!read_mmio) {
		pr_info("[cpu-oc-diag] policy%u: MMIO read disabled\n", cpu);
		goto out;
	}
	for (i = 0; i < (unsigned int)data->nr_opp; i++) {
		raw = readl_relaxed(lut_base + i * DIAG_LUT_ROW_SIZE);
		freq = FIELD_GET(DIAG_LUT_FREQ, raw) * 1000;
		pr_info("[cpu-oc-diag] policy%u lut[%u]=%08x freq=%u table=%u\n",
			cpu, i, raw, freq, table[i].frequency);
	}
	pr_info("[cpu-oc-diag] policy%u enable=%08x perf_state=%08x hw_state=%08x\n",
		cpu, readl_relaxed(data->reg_bases[DIAG_REG_FREQ_ENABLE]),
		readl_relaxed(data->reg_bases[DIAG_REG_FREQ_PERF_STATE]),
		readl_relaxed(data->reg_bases[DIAG_REG_FREQ_HW_STATE]));

out:
	up_read(&policy->rwsem);
	cpufreq_cpu_put(policy);
}

static void diag_resolve_probe(void *unused, struct cpufreq_policy *policy,
			       unsigned int *target_freq,
			       unsigned int old_target_freq)
{
	unsigned int count;
	unsigned int clamped;
	struct cpufreq_frequency_table *table;

	if (!policy || !target_freq)
		return;
	clamped = READ_ONCE(*target_freq);
	table = READ_ONCE(policy->freq_table);
	count = atomic_inc_return(&trace_count);
	if (count > 24)
		return;
	pr_info("[cpu-oc-diag] resolve cpu=%u old=%u target=%u->%u max=%u cpuinfo_max=%u table0=%u\n",
		READ_ONCE(policy->cpu), old_target_freq, old_target_freq, clamped,
		READ_ONCE(policy->max), READ_ONCE(policy->cpuinfo.max_freq),
		table ? READ_ONCE(table[0].frequency) : 0);
}

static int __init diag_init(void)
{
	int ret = 0;

	pr_info("[cpu-oc-diag] read-only diagnostic loaded\n");
	if (trace_probe) {
		ret = register_trace_android_vh_cpufreq_resolve_freq(
			diag_resolve_probe, NULL);
		if (ret) {
			pr_err("[cpu-oc-diag] cannot register resolve probe: %d\n", ret);
			return ret;
		}
		pr_info("[cpu-oc-diag] resolve probe registered\n");
		trace_registered = true;
	}

	if (trace_qos) {
		ret = register_trace_android_vh_freq_qos_update_request(
			diag_qos_probe, NULL);
		if (ret) {
			pr_err("[cpu-oc-diag] cannot register qos probe: %d\n", ret);
			goto err;
		}
		pr_info("[cpu-oc-diag] qos probe registered\n");
		trace_qos_registered = true;
	}

	if (trace_qosup) {
		qosup_kp.symbol_name = "freq_qos_update_request";
		qosup_kp.pre_handler = qosup_pre;
		ret = register_kprobe(&qosup_kp);
		if (ret) {
			pr_err("[cpu-oc-diag] cannot register qos_update kprobe: %d\n",
			       ret);
			goto err;
		}
		pr_info("[cpu-oc-diag] qos_update kprobe registered at %px\n",
			qosup_kp.addr);
		qosup_registered = true;
	}

	diag_policy(0);
	diag_policy(6);
	diag_opp(0);
	diag_opp(6);
	diag_regulator(0);
	diag_regulator(6);
	diag_lut_phys();
	return 0;

err:
	if (qosup_registered) {
		unregister_kprobe(&qosup_kp);
		qosup_registered = false;
	}
	if (trace_qos_registered) {
		unregister_trace_android_vh_freq_qos_update_request(
			diag_qos_probe, NULL);
		tracepoint_synchronize_unregister();
		trace_qos_registered = false;
	}
	if (trace_registered) {
		unregister_trace_android_vh_cpufreq_resolve_freq(
			diag_resolve_probe, NULL);
		tracepoint_synchronize_unregister();
		trace_registered = false;
	}
	return ret;
}

static void __exit diag_exit(void)
{
	if (lut_map) {
		iounmap(lut_map);
		lut_map = NULL;
	}
	if (trace_qos_registered) {
		unregister_trace_android_vh_freq_qos_update_request(
			diag_qos_probe, NULL);
		tracepoint_synchronize_unregister();
		trace_qos_registered = false;
		pr_info("[cpu-oc-diag] qos probe unregistered\n");
	}
	if (trace_registered) {
		unregister_trace_android_vh_cpufreq_resolve_freq(
			diag_resolve_probe, NULL);
		tracepoint_synchronize_unregister();
		trace_registered = false;
		pr_info("[cpu-oc-diag] resolve probe unregistered\n");
	}
	if (qosup_registered) {
		unregister_kprobe(&qosup_kp);
		qosup_registered = false;
		pr_info("[cpu-oc-diag] qos_update kprobe unregistered\n");
	}
	pr_info("[cpu-oc-diag] unloaded\n");
}

module_init(diag_init);
module_exit(diag_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MT6789 read-only cpufreq policy, hardware LUT, and regulator diagnostic");
