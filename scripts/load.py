#!/usr/bin/env python3
import argparse
import glob
import os
import subprocess
import sys

DEFAULT_SYSFS_CPUFREQ = "/sys/devices/system/cpu/cpufreq"
ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DEFAULT_KO = os.path.join(ROOT_DIR, "src", "cpu_oc_mt6789.ko")
DEFAULT_DIAG_KO = os.path.join(ROOT_DIR, "src", "cpu_oc_diag_mt6789.ko")
DEFAULT_NOOP_KO = os.path.join(ROOT_DIR, "src", "cpu_oc_noop_mt6789.ko")


def get_cpufreq_policies(sysfs_root=DEFAULT_SYSFS_CPUFREQ):
    pattern = os.path.join(sysfs_root, "policy*")
    dirs = sorted(glob.glob(pattern))
    policies = []
    for d in dirs:
        if not os.path.isdir(d):
            continue
        policies.append({"path": d, "name": os.path.basename(d)})
    return policies


def drop_cpu_frequencies(sysfs_root=DEFAULT_SYSFS_CPUFREQ):
    policies = get_cpufreq_policies(sysfs_root)
    for p in policies:
        min_freq = None
        for cand in ("cpuinfo_min_freq", "scaling_min_freq"):
            path = os.path.join(p["path"], cand)
            if os.path.isfile(path):
                try:
                    with open(path, "r") as f:
                        content = f.read().strip()
                        if content:
                            min_freq = content
                            break
                except OSError:
                    pass
        if min_freq:
            max_path = os.path.join(p["path"], "scaling_max_freq")
            try:
                with open(max_path, "w") as f:
                    f.write(min_freq + "\n")
            except OSError:
                pass


def restore_cpu_frequencies(sysfs_root=DEFAULT_SYSFS_CPUFREQ):
    policies = get_cpufreq_policies(sysfs_root)
    for p in policies:
        max_freq = None
        for cand in ("cpuinfo_max_freq", "scaling_available_frequencies"):
            path = os.path.join(p["path"], cand)
            if os.path.isfile(path):
                try:
                    with open(path, "r") as f:
                        content = f.read().strip()
                        if cand == "cpuinfo_max_freq" and content:
                            max_freq = content
                            break
                        elif cand == "scaling_available_frequencies" and content:
                            freqs = content.split()
                            if freqs:
                                max_freq = freqs[-1]
                                break
                except OSError:
                    pass
        if max_freq:
            max_path = os.path.join(p["path"], "scaling_max_freq")
            try:
                with open(max_path, "w") as f:
                    f.write(max_freq + "\n")
            except OSError:
                pass


def parse_kallsyms_stream(stream, targets):
    results = {k: None for k in targets}
    remaining = len(targets)
    sym_map = {}
    for key, (sym, mod) in targets.items():
        sym_map.setdefault(sym, []).append((key, mod))

    for line in stream:
        parts = line.strip().split()
        if len(parts) < 3:
            continue
        addr = parts[0]
        sym = parts[2]
        mod = None
        if len(parts) >= 4 and parts[3].startswith("[") and parts[3].endswith("]"):
            mod = parts[3][1:-1]

        if sym in sym_map:
            for key, target_mod in sym_map[sym]:
                if results[key] is not None:
                    continue
                if target_mod and mod:
                    if target_mod == mod:
                        results[key] = "0x" + addr
                        remaining -= 1
                elif target_mod is None or mod is None:
                    results[key] = "0x" + addr
                    remaining -= 1
        if remaining <= 0:
            break

    for k in results:
        if results[k] is None:
            results[k] = "0"
    return results


class KptrRestrictGuard:
    def __init__(self, path="/proc/sys/kernel/kptr_restrict"):
        self.path = path
        self.orig_val = None

    def __enter__(self):
        if os.path.isfile(self.path):
            try:
                with open(self.path, "r") as f:
                    self.orig_val = f.read().strip()
                with open(self.path, "w") as f:
                    f.write("0\n")
            except OSError:
                pass
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        if self.orig_val is not None and os.path.isfile(self.path):
            try:
                with open(self.path, "w") as f:
                    f.write(self.orig_val + "\n")
            except OSError:
                pass


def run_cmd(cmd, check=True):
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if check and res.returncode != 0:
        sys.stderr.write(f"command failed ({res.returncode}): {' '.join(cmd)}\n")
        if res.stderr:
            sys.stderr.write(res.stderr + "\n")
        sys.exit(res.returncode)
    return res


def load_module(module_path, params=None, force=False):
    cmd = ["insmod"]
    if force:
        cmd.append("-f")
    cmd.append(module_path)
    if params:
        for k, v in params.items():
            cmd.append(f"{k}={v}")
    run_cmd(cmd)


def unload_module(mod_name):
    run_cmd(["rmmod", mod_name], check=False)


def print_dmesg(filter_str):
    res = subprocess.run(["dmesg"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res.returncode == 0:
        lines = [line for line in res.stdout.splitlines() if filter_str in line]
        for line in lines[-20:]:
            print(line)


def dump_policies():
    for pol in (6, 0):
        pol_dir = f"/sys/devices/system/cpu/cpufreq/policy{pol}"
        print(f"--- policy{pol} ---")
        for name in ("cpuinfo_max_freq", "scaling_max_freq", "scaling_available_frequencies"):
            fpath = os.path.join(pol_dir, name)
            if os.path.isfile(fpath):
                try:
                    with open(fpath) as f:
                        print(f"{name}={f.read().strip()}")
                except OSError:
                    print(f"{name}=<error>")
            else:
                print(f"{name}=<missing>")


def do_load(args):
    ko = os.path.abspath(args.ko)
    if not os.path.isfile(ko):
        sys.exit(f"missing module: {ko}")

    targets = {
        "touch_max_ptr": ("freq_max_request", "touch_boost"),
        "powerhal_max_ptr": ("freq_max_request", "powerhal_cpu_ctrl"),
        "fpsgo_max_ptr": ("fbt_max_freq", "mtk_fpsgo_v3"),
        "fpsgo_cur_ptr": ("fbt_cur_ceiling", "mtk_fpsgo_v3"),
        "fpsgo_rq_ptr": ("fbt_cpu_rq", "mtk_fpsgo_v3"),
    }

    with KptrRestrictGuard():
        if os.path.isfile("/proc/kallsyms"):
            with open("/proc/kallsyms", "r") as f:
                addrs = parse_kallsyms_stream(f, targets)
        else:
            addrs = {k: "0" for k in targets}

    print(
        f"TOUCH={addrs['touch_max_ptr']} POWERHAL={addrs['powerhal_max_ptr']} "
        f"FPSGO_MAX={addrs['fpsgo_max_ptr']} FPSGO_CUR={addrs['fpsgo_cur_ptr']} "
        f"FPSGO_RQ={addrs['fpsgo_rq_ptr']}"
    )

    if all(addrs[k] in ("0", "0x0000000000000000") for k in addrs):
        sys.exit("addresses hidden or unresolved; check kptr_restrict or symbols")

    print("Dropping CPU cores to minimum frequency...")
    drop_cpu_frequencies()

    subprocess.run(["dmesg", "-c"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    params = {
        "touch_max_ptr": addrs["touch_max_ptr"],
        "powerhal_max_ptr": addrs["powerhal_max_ptr"],
        "fpsgo_max_ptr": addrs["fpsgo_max_ptr"],
        "fpsgo_cur_ptr": addrs["fpsgo_cur_ptr"],
        "fpsgo_rq_ptr": addrs["fpsgo_rq_ptr"],
        "drop_powerhal": "1" if args.drop_powerhal else "0",
    }
    load_module(ko, params=params, force=args.force)
    print_dmesg("cpu-oc")

    print("Maximizing scaling_max_freq...")
    restore_cpu_frequencies()
    dump_policies()


def do_unload(args):
    print("Dropping CPU cores to minimum frequency before unload...")
    drop_cpu_frequencies()

    unload_module("cpu_oc_mt6789")
    print_dmesg("cpu-oc")

    print("Restoring scaling_max_freq after unload...")
    restore_cpu_frequencies()
    dump_policies()


def do_diag(args):
    ko = os.path.abspath(args.ko or DEFAULT_DIAG_KO)
    if not os.path.isfile(ko):
        sys.exit(f"missing diag module: {ko}")
    subprocess.run(["dmesg", "-c"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    params = {
        "trace_probe": "0",
        "trace_qos": "0",
        "trace_qosup": "0",
        "read_driver": "0",
        "read_mmio": "0",
        "read_opp": "1",
        "read_regulator": "1",
        "regulator_name": "cpu",
        "lut_phys": "0",
    }
    load_module(ko, params=params, force=args.force)
    print_dmesg("cpu-oc-diag")


def do_diag_unload(args):
    unload_module("cpu_oc_diag_mt6789")
    print_dmesg("cpu-oc-diag")


def do_noop(args):
    ko = os.path.abspath(args.ko or DEFAULT_NOOP_KO)
    if not os.path.isfile(ko):
        sys.exit(f"missing noop module: {ko}")
    subprocess.run(["dmesg", "-c"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    load_module(ko, force=args.force)
    print_dmesg("cpu-oc-noop")


def do_noop_unload(args):
    unload_module("cpu_oc_noop_mt6789")
    print_dmesg("cpu-oc-noop")


def main():
    parser = argparse.ArgumentParser(description="MT6789 CPU OC Loader and Controller")
    subparsers = parser.add_subparsers(dest="action")

    p_load = subparsers.add_parser("load", help="Load CPU OC module")
    p_load.add_argument("ko", nargs="?", default=DEFAULT_KO, help="Path to cpu_oc_mt6789.ko")
    p_load.add_argument("-f", "--force", action="store_true", help="Force load module (insmod -f)")
    p_load.add_argument("--drop-powerhal", action="store_true", help="Drop powerhal QoS limits")

    subparsers.add_parser("unload", help="Unload CPU OC module")

    p_diag = subparsers.add_parser("diag", help="Load diag module")
    p_diag.add_argument("ko", nargs="?", default=DEFAULT_DIAG_KO, help="Path to diag.ko")
    p_diag.add_argument("-f", "--force", action="store_true", help="Force load module")

    subparsers.add_parser("diag-unload", help="Unload diag module")

    p_noop = subparsers.add_parser("noop", help="Load noop module")
    p_noop.add_argument("ko", nargs="?", default=DEFAULT_NOOP_KO, help="Path to noop.ko")
    p_noop.add_argument("-f", "--force", action="store_true", help="Force load module")

    subparsers.add_parser("noop-unload", help="Unload noop module")

    if len(sys.argv) == 1:
        args = parser.parse_args(["load"])
    elif sys.argv[1] not in (
        "load",
        "unload",
        "diag",
        "diag-unload",
        "noop",
        "noop-unload",
        "-h",
        "--help",
    ):
        args = parser.parse_args(["load"] + sys.argv[1:])
    else:
        args = parser.parse_args()

    dispatch = {
        "load": do_load,
        "unload": do_unload,
        "diag": do_diag,
        "diag-unload": do_diag_unload,
        "noop": do_noop,
        "noop-unload": do_noop_unload,
    }

    if args.action in dispatch:
        dispatch[args.action](args)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
