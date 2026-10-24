import io
import os
import shutil
import tempfile
import unittest

# This will import once scripts/load.py is implemented
try:
    from scripts.load import (
        drop_cpu_frequencies,
        get_cpufreq_policies,
        parse_kallsyms_stream,
        restore_cpu_frequencies,
    )
except ImportError:
    drop_cpu_frequencies = None
    get_cpufreq_policies = None
    parse_kallsyms_stream = None
    restore_cpu_frequencies = None


class TestKallsymsStreaming(unittest.TestCase):
    def test_parse_kallsyms_stream_matches_and_stops_early(self):
        sample_kallsyms = (
            "ffffff8008000000 t some_early_func\n"
            "ffffff8009123450 d freq_max_request [touch_boost]\n"
            "ffffff8009654320 d freq_max_request [powerhal_cpu_ctrl]\n"
            "ffffff800a111110 b fbt_max_freq [mtk_fpsgo_v3]\n"
            "ffffff800a222220 b fbt_cur_ceiling [mtk_fpsgo_v3]\n"
            "ffffff800a333330 b fbt_cpu_rq [mtk_fpsgo_v3]\n"
            "ffffff800b999990 t never_reached_func\n"
            "ffffff800c000000 t another_func\n"
        )
        stream = io.StringIO(sample_kallsyms)
        targets = {
            "touch_max_ptr": ("freq_max_request", "touch_boost"),
            "powerhal_max_ptr": ("freq_max_request", "powerhal_cpu_ctrl"),
            "fpsgo_max_ptr": ("fbt_max_freq", "mtk_fpsgo_v3"),
            "fpsgo_cur_ptr": ("fbt_cur_ceiling", "mtk_fpsgo_v3"),
            "fpsgo_rq_ptr": ("fbt_cpu_rq", "mtk_fpsgo_v3"),
        }

        results = parse_kallsyms_stream(stream, targets)
        self.assertEqual(results["touch_max_ptr"], "0xffffff8009123450")
        self.assertEqual(results["powerhal_max_ptr"], "0xffffff8009654320")
        self.assertEqual(results["fpsgo_max_ptr"], "0xffffff800a111110")
        self.assertEqual(results["fpsgo_cur_ptr"], "0xffffff800a222220")
        self.assertEqual(results["fpsgo_rq_ptr"], "0xffffff800a333330")

        # Verify early exit: the stream should not have been read to EOF
        remaining = stream.read()
        self.assertIn("another_func", remaining)

    def test_parse_kallsyms_stream_global_fallback(self):
        sample_kallsyms = (
            "ffffff8008000000 t some_early_func\n"
            "ffffff8009123450 d freq_max_request\n"
        )
        stream = io.StringIO(sample_kallsyms)
        targets = {
            "touch_max_ptr": ("freq_max_request", "touch_boost"),
        }
        results = parse_kallsyms_stream(stream, targets)
        self.assertEqual(results["touch_max_ptr"], "0xffffff8009123450")


class TestCpufreqSysfsManagement(unittest.TestCase):
    def setUp(self):
        self.test_dir = tempfile.mkdtemp()
        self.policy0 = os.path.join(self.test_dir, "policy0")
        self.policy6 = os.path.join(self.test_dir, "policy6")
        os.makedirs(self.policy0)
        os.makedirs(self.policy6)

        with open(os.path.join(self.policy0, "cpuinfo_min_freq"), "w") as f:
            f.write("500000\n")
        with open(os.path.join(self.policy0, "cpuinfo_max_freq"), "w") as f:
            f.write("2000000\n")
        with open(os.path.join(self.policy0, "scaling_max_freq"), "w") as f:
            f.write("2000000\n")

        with open(os.path.join(self.policy6, "cpuinfo_min_freq"), "w") as f:
            f.write("725000\n")
        with open(os.path.join(self.policy6, "cpuinfo_max_freq"), "w") as f:
            f.write("2200000\n")
        with open(os.path.join(self.policy6, "scaling_max_freq"), "w") as f:
            f.write("2200000\n")

    def tearDown(self):
        shutil.rmtree(self.test_dir)

    def test_get_cpufreq_policies(self):
        policies = get_cpufreq_policies(self.test_dir)
        self.assertEqual(len(policies), 2)
        paths = [p["path"] for p in policies]
        self.assertIn(self.policy0, paths)
        self.assertIn(self.policy6, paths)

    def test_drop_cpu_frequencies(self):
        drop_cpu_frequencies(self.test_dir)
        with open(os.path.join(self.policy0, "scaling_max_freq")) as f:
            self.assertEqual(f.read().strip(), "500000")
        with open(os.path.join(self.policy6, "scaling_max_freq")) as f:
            self.assertEqual(f.read().strip(), "725000")

    def test_restore_cpu_frequencies(self):
        # Set them low first
        drop_cpu_frequencies(self.test_dir)
        restore_cpu_frequencies(self.test_dir)
        with open(os.path.join(self.policy0, "scaling_max_freq")) as f:
            self.assertEqual(f.read().strip(), "2000000")
        with open(os.path.join(self.policy6, "scaling_max_freq")) as f:
            self.assertEqual(f.read().strip(), "2200000")


if __name__ == "__main__":
    unittest.main()
