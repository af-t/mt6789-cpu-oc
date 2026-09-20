#include <linux/module.h>

static int __init noop_init(void)
{
	pr_info("[cpu-oc-noop] loaded\n");
	return 0;
}

static void __exit noop_exit(void)
{
	pr_info("[cpu-oc-noop] unloaded\n");
}

module_init(noop_init);
module_exit(noop_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MT6789 no-op module for loader isolation");
