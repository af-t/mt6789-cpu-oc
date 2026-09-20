/* Build-time defaults. Rebuild after editing; load-time args win. */
#ifndef OC_PROFILE_H
#define OC_PROFILE_H

/* Stock maxima (kHz). Must match the device. */
#define PROFILE_STOCK_BIG_MAX		2200000
#define PROFILE_STOCK_LITTLE_MAX	2000000

/* From gen_table.py; fallback if oc_targets.h absent. 0 = skip. */
#if __has_include("oc_targets.h")
#include "oc_targets.h"
#else
#define PROFILE_BIG_OC			2400000
#define PROFILE_LITTLE_OC		2200000
#define PROFILE_STEP_GUARD		200000
#endif

/* MTK cpufreq-hw LUT bases (DT physical). 0 = skip LUT patch. */
#define PROFILE_LUT_PHYS_BIG		0x11bd30UL
#define PROFILE_LUT_PHYS_LITTLE		0x11bc10UL

/* Require stock < oc <= stock + guard, oc <= abs cap. */
#define PROFILE_ABS_CAP			2700000

/* Feature defaults (0/1). */
#define PROFILE_PATCH_LUT		1
#define PROFILE_HOOK_OVERRIDE		1
#define PROFILE_DROP_POWERHAL		0
#define PROFILE_IGNORE_VERIFY		0

#endif /* OC_PROFILE_H */
