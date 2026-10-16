#include <linux/module.h>
#include <linux/io.h>
#include <linux/slab.h>

struct emimpu_region {
	unsigned long start;
	unsigned long end;
	unsigned int region;
	unsigned int lock;
	unsigned int *d_apc;
};

extern int mtk_emimpu_init_region(struct emimpu_region *region, unsigned int region_id);
extern int mtk_emimpu_set_addr(struct emimpu_region *region, unsigned long start, unsigned long end);
extern int mtk_emimpu_set_apc(struct emimpu_region *region, unsigned int domain, unsigned int apc);
extern int mtk_emimpu_set_protection(struct emimpu_region *region);
extern int mtk_emimpu_free_region(struct emimpu_region *region);

#define MCUPM_PHYS_BASE 0x7fea0000UL
#define MCUPM_SIZE      0x100000UL
#define MCUPM_REGION_ID 0x13U

static int __init emi_test_init(void)
{
	struct emimpu_region r;
	void __iomem *vbase;
	u32 magic;
	int ret;

	pr_info("[emi-unlock-test] starting MCUPM unlock test on region 0x%x\n", MCUPM_REGION_ID);

	ret = mtk_emimpu_init_region(&r, MCUPM_REGION_ID);
	if (ret) {
		pr_err("[emi-unlock-test] mtk_emimpu_init_region failed: %d\n", ret);
		return ret;
	}

	mtk_emimpu_set_addr(&r, MCUPM_PHYS_BASE, MCUPM_PHYS_BASE + MCUPM_SIZE - 1);
	mtk_emimpu_set_apc(&r, 0, 3);
	mtk_emimpu_set_apc(&r, 14, 3);

	ret = mtk_emimpu_set_protection(&r);
	mtk_emimpu_free_region(&r);

	pr_info("[emi-unlock-test] set_protection returned: %d\n", ret);
	if (ret) {
		pr_err("[emi-unlock-test] ATF rejected unlock (ret=%d), aborting read to prevent panic\n", ret);
		return ret;
	}

	vbase = ioremap(MCUPM_PHYS_BASE, 0x1000);
	if (!vbase) {
		pr_err("[emi-unlock-test] ioremap %lx failed\n", MCUPM_PHYS_BASE);
		return -ENOMEM;
	}

	magic = readl_relaxed(vbase);
	pr_info("[emi-unlock-test] SUCCESS! Read MCUPM magic: 0x%08x\n", magic);

	iounmap(vbase);
	return 0;
}

static void __exit emi_test_exit(void)
{
	struct emimpu_region r;
	int ret;

	ret = mtk_emimpu_init_region(&r, MCUPM_REGION_ID);
	if (!ret) {
		mtk_emimpu_set_addr(&r, MCUPM_PHYS_BASE, MCUPM_PHYS_BASE + MCUPM_SIZE - 1);
		mtk_emimpu_set_apc(&r, 0, 0);
		mtk_emimpu_set_apc(&r, 14, 0);
		mtk_emimpu_set_protection(&r);
		mtk_emimpu_free_region(&r);
	}
	pr_info("[emi-unlock-test] restored original protection\n");
}

module_init(emi_test_init);
module_exit(emi_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MT6789 EMI MPU Unlock Test for MCUPM");
