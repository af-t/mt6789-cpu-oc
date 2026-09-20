#!/bin/sh
# Load cpu_oc_mt6789.ko (frequency tuning is baked at build time, while
# feature switches can be selected at load time).
# KASLR re-randomizes module bases every boot, so the vendor pointer
# addresses must be read live after lowering kptr_restrict (resets on reboot).
# Usage (as root):
#   sh scripts/load-oc.sh [path/to/cpu_oc_mt6789.ko]
#   sh scripts/load-oc.sh unload | diag | diag-unload | noop | noop-unload
set -eu

ROOT=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
KO=${KO:-"$ROOT/src/cpu_oc_mt6789.ko"}
DIAG_KO=${DIAG_KO:-"$ROOT/src/cpu_oc_diag_mt6789.ko"}
NOOP_KO=${NOOP_KO:-"$ROOT/src/cpu_oc_noop_mt6789.ko"}
INSMod=${INSMod:-insmod}
RMMOD=${RMMOD:-rmmod}
TRACE_PROBE=${TRACE_PROBE:-0}
TRACE_QOS=${TRACE_QOS:-0}
TRACE_QOSUP=${TRACE_QOSUP:-0}
READ_DRIVER=${READ_DRIVER:-0}
READ_MMIO=${READ_MMIO:-0}
READ_OPP=${READ_OPP:-1}
READ_REGULATOR=${READ_REGULATOR:-1}
REGULATOR_NAME=${REGULATOR_NAME:-cpu}
LUT_PHYS=${LUT_PHYS:-0}
FORCE=${FORCE:-0}
FORCE_ARGS=${FORCE_ARGS:-}
DROP_POWERHAL=${DROP_POWERHAL:-0}

supports_force() {
	case $("$INSMod" --help 2>&1 || true) in
		*-f*|*force*) return 0 ;;
		*) return 1 ;;
	esac
}

load_module() {
	module=$1
	shift
	if [ "$FORCE" = 1 ]; then
		if [ -n "$FORCE_ARGS" ]; then
			"$INSMod" $FORCE_ARGS "$module" "$@"
		elif supports_force; then
			"$INSMod" -f "$module" "$@"
		else
			echo "warning: $INSMod has no force option; loading without force" >&2
			"$INSMod" "$module" "$@"
		fi
	else
		"$INSMod" "$module" "$@"
	fi
}

if [ "${1:-}" = unload ]; then
	"$RMMOD" cpu_oc_mt6789
	dmesg | grep cpu-oc || true
	exit 0
fi

if [ "${1:-}" = diag ]; then
	[ -f "$DIAG_KO" ] || { echo "missing: $DIAG_KO"; exit 1; }
	dmesg -c > /dev/null 2>&1 || true
	load_module "$DIAG_KO" trace_probe="$TRACE_PROBE" trace_qos="$TRACE_QOS" trace_qosup="$TRACE_QOSUP" read_driver="$READ_DRIVER" read_mmio="$READ_MMIO" read_opp="$READ_OPP" read_regulator="$READ_REGULATOR" regulator_name="$REGULATOR_NAME" lut_phys="$LUT_PHYS"
	dmesg -c | grep cpu-oc-diag || true
	exit 0
fi

if [ "${1:-}" = diag-unload ]; then
	"$RMMOD" cpu_oc_diag_mt6789
	dmesg | grep cpu-oc-diag || true
	exit 0
fi

if [ "${1:-}" = noop ]; then
	[ -f "$NOOP_KO" ] || { echo "missing: $NOOP_KO"; exit 1; }
	dmesg -c > /dev/null 2>&1 || true
	load_module "$NOOP_KO"
	dmesg -c | grep cpu-oc-noop || true
	exit 0
fi

if [ "${1:-}" = noop-unload ]; then
	"$RMMOD" cpu_oc_noop_mt6789
	dmesg | grep cpu-oc-noop || true
	exit 0
fi

KO=${1:-"$KO"}
[ -f "$KO" ] || { echo "missing: $KO"; exit 1; }

KPTR=$(cat /proc/sys/kernel/kptr_restrict)
echo 0 > /proc/sys/kernel/kptr_restrict
trap 'echo "$KPTR" > /proc/sys/kernel/kptr_restrict' EXIT

kaddr() {
	cat /proc/kallsyms 2>/dev/null | awk -v s="$1" -v m="[$2]" \
		'$3 == s && $4 == m { print "0x" $1; exit }'
}

TOUCH_MAX_PTR=$(kaddr freq_max_request touch_boost) || true
POWERHAL_MAX_PTR=$(kaddr freq_max_request powerhal_cpu_ctrl) || true
FPSGO_MAX_PTR=$(kaddr fbt_max_freq mtk_fpsgo_v3) || true
FPSGO_CUR_PTR=$(kaddr fbt_cur_ceiling mtk_fpsgo_v3) || true
FPSGO_RQ_PTR=$(kaddr fbt_cpu_rq mtk_fpsgo_v3) || true
[ -z "$TOUCH_MAX_PTR" ] && TOUCH_MAX_PTR=0
[ -z "$POWERHAL_MAX_PTR" ] && POWERHAL_MAX_PTR=0
[ -z "$FPSGO_MAX_PTR" ] && FPSGO_MAX_PTR=0
[ -z "$FPSGO_CUR_PTR" ] && FPSGO_CUR_PTR=0
[ -z "$FPSGO_RQ_PTR" ] && FPSGO_RQ_PTR=0
echo "TOUCH=$TOUCH_MAX_PTR POWERHAL=$POWERHAL_MAX_PTR FPSGO_MAX=$FPSGO_MAX_PTR FPSGO_CUR=$FPSGO_CUR_PTR FPSGO_RQ=$FPSGO_RQ_PTR"
case "$TOUCH_MAX_PTR $POWERHAL_MAX_PTR $FPSGO_MAX_PTR $FPSGO_CUR_PTR $FPSGO_RQ_PTR" in
  *0x0000000000000000*|"0 0 0 0 0") echo "addresses hidden, kptr_restrict stuck?"; exit 1;;
esac

dmesg -c > /dev/null 2>&1 || true
load_module "$KO" touch_max_ptr="$TOUCH_MAX_PTR" powerhal_max_ptr="$POWERHAL_MAX_PTR" fpsgo_max_ptr="$FPSGO_MAX_PTR" fpsgo_cur_ptr="$FPSGO_CUR_PTR" fpsgo_rq_ptr="$FPSGO_RQ_PTR" drop_powerhal="$DROP_POWERHAL"
dmesg -c | grep cpu-oc || true

for policy in 6 0; do
	echo "--- policy$policy ---"
	for name in cpuinfo_max_freq scaling_max_freq scaling_available_frequencies; do
		path="/sys/devices/system/cpu/cpufreq/policy$policy/$name"
		if [ -r "$path" ]; then
			printf '%s=' "$name"
			cat "$path"
		else
			echo "$name=<missing>"
		fi
	done
done
