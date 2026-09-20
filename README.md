# MT6789 CPU OC (LKM experiment, stock mcupm)

Experimental CPU overclock for the MT6789 (Helio G99), implemented as a
loadable kernel module. The module patches the cpufreq policy, QoS votes,
frequency table, and the MediaTek hardware LUT, then restores the original
state on `rmmod`.
Verified on device: big 2200000 -> 2500000 kHz, little 2000000 -> 2300000 kHz,
with `scaling_cur_freq` reaching the OC values under load.

## Why an LKM (and not an mcupm patch)

The stock DVFS brain lives in the `mcupm`/`pi_img` firmware on protected
partitions. Writing there is blocked on this device (`baseband_guard: deny
write to protected partition`), and flashing those partitions anyway is
unhealthy: it risks a hard brick, breaks firmware signatures, and any
mistake persists across reboots with no easy undo.

This project exists to avoid all of that. It changes nothing on disk and
nothing in firmware: every mutation happens at runtime through a kernel
module, every mutation has an inverse, and `rmmod` (or a reboot) leaves
zero residue. If a variant misbehaves, you unload it instead of
reflashing anything.

## How the cap actually works (measured, not hypothesized)

- `policy->max` can only ratchet down through `__resolve_freq`: the table
  lookup re-clamps to the assigned max, so raising limits requires every
  effective QoS voter to be at/above the target first.
- Three vendor voters cap the big cluster at stock: `touch_boost` (static
  core request from boot), `powerhal_cpu_ctrl` (rewritten by the userspace
  perf daemon via proc), `mtk_fpsgo_v3` (refreshes from its `fbt_max_freq` /
  `fbt_cur_ceiling` cache).
- The module therefore assists all three: bump touch + fpsgo core requests,
  data-patch the fpsgo cache (validate + backup + restore, GPU-style), and
  optionally remove the powerhal requests so daemon rewrites become harmless
  no-ops (`drop_powerhal=1`, restored on unload; default is 0).
- `hook_override=1` keeps a surgical resolve-hook (gap values only) as a
  backstop; `patch_lut=1` writes HW LUT row 0 via DT physical bases
  (little `0x11bc10`, big `0x11bd30`). `policy->driver_data` is never
  touched (its layout differs from source on this device and caused the
  earlier panic).

## Known limits (measured)

- Frequency-only OC tops out around +300 MHz (2500/2300) on stock voltage.
  2600/2400 loads but freezes under load; 2700/2500 dies instantly.
- Voltage is owned by the mcupm/hardware DVFS path, not by Linux regulator
  consumers: a paired Vproc/Vsram floor boost was implemented, but the
  regulator coupler silently capped it (`max_spread`), the hardware kept
  moving the rails under load, and releasing the floor on unload raced an
  in-flight OC frequency and froze the phone. The approach was reverted.
  Anything past +300 MHz needs an `mcupm` change, which is exactly what
  this project refuses to do (see above).

## Build

Use the prepared target kernel tree:

```sh
make KDIR=/var/tmp/kernel-6.12-2025-09 ARCH=arm64 LLVM=1
llvm-strip --strip-debug src/cpu_oc_mt6789.ko
```

The tree has no complete `Module.symvers`, so modpost warnings are expected.
Toybox `insmod` has no `-f`; the helper loads without force in that case.

## Load and restore

Pointer parameters (`touch_max_ptr`, `powerhal_max_ptr`, `fpsgo_max_ptr`,
`fpsgo_cur_ptr`, `fpsgo_rq_ptr`) auto-resolve from `/proc/kallsyms` at every
load (module ASLR changes them each boot); pass `KO=path` or explicit
`0x...` values only to override. Targets are capped at stock + `step_guard`
and rejected early otherwise.

Pick a variant, generate its baked targets, rebuild, and load by path
(tuning lives in the build, not in loader arguments):

```sh
./tools/gen_table.py 2500 2300 > src/oc_targets.h
make KDIR=/var/tmp/kernel-6.12-2025-09 ARCH=arm64 LLVM=1
llvm-strip --strip-debug src/cpu_oc_mt6789.ko
sh scripts/load-oc.sh src/cpu_oc_mt6789.ko
cat /sys/devices/system/cpu/cpufreq/policy6/scaling_cur_freq
cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq
```

To experiment with your own configuration, edit `src/oc_profile.h`
(stock maxima, LUT bases, feature defaults); targets and guard come
from the generated `src/oc_targets.h`. The table is retuned in place (whole
curve shifted by the OC delta, entry count and minimum anchor unchanged),
never lengthened.

`DROP_POWERHAL=1` enables the powerhal request-removal workaround; the
default is 0 so powerhal voters remain intact. `IGNORE_VERIFY=1` keeps patches
loaded when the refresh check fails (debug aid; not needed when all voters
cooperate). Restore immediately with:

```sh
sh scripts/load-oc.sh unload
```

`rmmod` restores the table, cpuinfo, QoS votes, vendor requests, fpsgo cache,
and LUT rows. If any assist fails validation, init rolls back and refuses
to load.

## Read-only diagnostic

```sh
TRACE_QOSUP=1 sh scripts/load-oc.sh diag
dmesg | grep cpu-oc-diag
sh scripts/load-oc.sh diag-unload
```

`TRACE_QOSUP=1` kprobes `freq_qos_update_request` callers; `READ_OPP=1`
(the default) reads CPU OPP frequencies and target voltages through the OPP
core. `READ_REGULATOR=1` (the default) reads the live CPU regulator voltage;
set `REGULATOR_NAME` when the device uses a supply name other than `cpu`.
This remains available when the stock hardware cpufreq driver does not expose
an OPP table. `/proc/mtk_freq_qos/` (from the stock `cpudvfs` driver) lists
every voter with caller symbols and is the fastest way to confirm who holds a
cap.

## How this was built (and how to port it)

What to look at, in what order, and which techniques transfer to other
devices/kernels.

### Result in one paragraph

CPU overclock on the Helio G99 (MT6789) via a loadable kernel module:
big 2.2 -> 2.5 GHz, little 2.0 -> 2.3 GHz. It patches the cpufreq frequency
table, `cpuinfo.max_freq`, every vendor QoS vote that caps the target, and
the MediaTek hardware LUT — then restores everything on `rmmod`.

### 1. Recon first, code later

Everything starts from sysfs/proc on the running device (root shell):

- `/sys/devices/system/cpu/cpufreq/policy*/{cpuinfo_max_freq,scaling_max_freq,scaling_available_frequencies,scaling_cur_freq}`
  tells you stock max, the live cap, and whether the cap moves.
- On MediaTek, `/proc/mtk_freq_qos/max_freq_req` (created by the stock
  `cpudvfs` driver) lists **every max-frequency voter with its caller
  symbol**. This single file names your enemies: here it was `touch_boost`,
  `powerhal_cpu_ctrl`, and `mtk_fpsgo_v3`, all voting stock max.
- `/proc/kallsyms` (needs `kptr_restrict=0`) gives runtime addresses of
  vendor module variables for later.

If your kernel lacks the mtk proc files, walk
`policy->constraints.max_freq_constraint.requests` from a diagnostic module
instead — same information.

### 2. Decompile the vendor drivers (yes, a decoder is needed)

Pull the vendor `.ko` files (`/vendor_dlkm/lib/modules/`) and decompile
with Ghidra. You are looking for exactly four things per module:

1. **Where its vote value comes from.** Here: boot-time OPP snapshots
   (`cpu_opp_tbl`, `fbt_max_freq`/`fbt_cur_ceiling`) — i.e. cached stock
   maxima, never re-read.
2. **When it votes.** `touch_boost` votes once at init (static cap);
   `powerhal` votes whenever userspace writes its proc file (daemon-driven,
   constantly re-stomped); `fpsgo` re-votes from cache on a fast tick
   (a refresher you can turn into an ally by patching the cache).
3. **The request object layout.** All three keep `struct freq_qos_request`
   arrays with 0x40-byte stride, indexed by policy in CPU order — so the
   live request address is `*(bss_ptr) + idx * 0x40`, validated by
   `req->type == FREQ_QOS_MAX` before touching.
4. **Clamp logic (or lack of it).** `powerhal`'s proc write passes values
   straight to core QoS with no clamp — the cap lived in the *voters*, not
   in a limiter. The cpudvfs layer stores votes unclamped too; it only
   mirrors core state for display.

### 3. Read the core source for the two mechanisms that matter

Against the matching kernel tree (`drivers/cpufreq/cpufreq.c`,
`include/linux/cpufreq.h`, `kernel/power/qos.c`):

- `scaling_max_freq` store **is** a QoS update (`store_one` ->
  `freq_qos_update_request(policy->max_freq_req, val)`). There is no
  separate user limit in this kernel generation.
- `policy->max = __resolve_freq(qos_effective, RELATION_H)`, and both
  `__resolve_freq` and the table lookup clamp to the *currently assigned*
  max first. Consequence: the policy max ratchets down freely but can never
  be raised through this path while a voter holds it down — you must fix
  the voters, not fight the resolver. (A resolve vendor-hook can only paper
  over a gap of one OPP entry, and it must be conditioned to fire solely on
  high targets or it will pin the minimum too.)

### 4. Module techniques (all used here, all portable)

- **Table + cpuinfo patch** with backup/restore and strict validation
  (sorted-descending check, head/tail match stock). The table is retuned
  in place — every entry except the minimum anchor is shifted by the OC
  delta, so step density matches stock and the allocation size never
  changes. Never trust `policy->driver_data` layout — on this device it
  mismatches source and panics; the HW LUT base came from the device tree
  instead.
- **Request bump**: `freq_qos_update_request()` on a vendor request found
  via kallsyms. Auto-resolve addresses in the loader script at each boot
  (module ASLR changes them); keep manual override params.
- **Cache data-patch** (GPU-style): validate old value == stock, write,
  read back, restore on unload. Turns a fast refresher (fpsgo) from enemy
  to ally.
- **Request removal**: `freq_qos_remove_request()` on a vote whose owner
  re-stomps it (powerhal/daemon). Later owner updates then fail closed
  (`-EINVAL`, no re-add) while the module is loaded; re-add the saved
  value on unload.
- **HW LUT row 0** through `ioremap` of the DT physical base, low 12 bits
  are the frequency: validate stock before writing, verify after, restore
  on unload.

### 5. Verify and restore discipline

Every mutation has an inverse, applied in reverse order on every failure
path — init aborts leave zero residue (proven by reload cycles). A
`refresh_and_verify` step re-reads table head, cpuinfo, policy max, and
the QoS effective value; targets are range-gated (`stock < oc <=
stock + guard`) before anything is touched. Variants are generated
(`tools/gen_table.py <big_MHz> <little_MHz> > src/oc_targets.h`), baked
at build time via `src/oc_profile.h`, and loaded by path
(`sh scripts/load-oc.sh src/cpu_oc_mt6789.ko`) — no tuning numbers at
load time.

### 6. Porting to another device/kernel

What changes and where to find it:

| Piece | Where it varies | How to find it |
|---|---|---|
| Stock maxima, table shape | DT/OPP + sysfs | `scaling_available_frequencies`, decompiled `cpu_policy_init` |
| Voter set | vendor modules | `/proc/mtk_freq_qos/*` or request-list walk |
| Request array stride/index | vendor decompile | `freq_qos_add_request` call sites |
| BSS variable names | vendor symbols | `kallsyms \| grep <driver>` |
| HW LUT base | device tree | decompiled driver + `driver_data` cross-check (or DT dump) |
| Resolve/clamp semantics | core version | `__resolve_freq` + `cpufreq_frequency_table_target` source |
| `STEP_GUARD` | silicon/voltage | raise in small steps, stability-test under load |

Things that bit us and may bite you: dynamic instruments (tracepoints,
kprobes) can stay silent on a given build — trust proc dumps and sysfs
behavior over probes; SELinux can flip sysfs writable nodes read-only
mid-session; the perf daemon rewrites votes on its own cadence, so prefer
removal/cache-patch over racing it.
