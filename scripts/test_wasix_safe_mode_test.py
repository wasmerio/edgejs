#!/usr/bin/env python3

import importlib.util
import unittest
from pathlib import Path


SCRIPT_PATH = Path(__file__).with_name("test-wasix-safe-mode.py")


def load_script():
    spec = importlib.util.spec_from_file_location("test_wasix_safe_mode", SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


NETWORK_CASE_NAMES = [
    "fetch http://example.com/",
    "https.get https://example.com/",
    "tls.connect verified example.com",
]

STATIC_DATA_CASE_NAMES = [
    "Intl.Collator de and DateTimeFormat de use ICU data",
    "TextDecoder shift_jis uses ICU converter data",
    "process.binding('natives') reads internal/deps sources",
    "fetch data: URL loads undici from /node-lib",
]

MISSING_ICU_CASE_NAME = "unusable ICU data directory falls back to the package mount"


class WasixSafeModeTests(unittest.TestCase):
    def test_build_cases_can_skip_external_network_checks(self):
        module = load_script()

        cases = module.build_cases("example.com", include_network=False)

        names = [case.name for case in cases]
        self.assertEqual(len(names), len(set(names)))
        self.assertIn("queueMicrotask", names)
        self.assertIn("blob.arrayBuffer", names)
        for name in NETWORK_CASE_NAMES:
            self.assertNotIn(name, names)

    def test_build_cases_includes_external_network_checks_when_requested(self):
        module = load_script()

        cases = module.build_cases("example.com", include_network=True)

        names = [case.name for case in cases]
        for name in NETWORK_CASE_NAMES:
            self.assertIn(name, names)

    def test_build_cases_cover_filesystem_backed_static_data(self):
        module = load_script()

        cases = module.build_cases("example.com", include_network=False)

        names = [case.name for case in cases]
        for name in STATIC_DATA_CASE_NAMES + [MISSING_ICU_CASE_NAME]:
            self.assertIn(name, names)
        missing_icu = next(case for case in cases if case.name == MISSING_ICU_CASE_NAME)
        self.assertEqual(missing_icu.expected_returncode, 0)
        self.assertEqual(missing_icu.env, (("EDGE_ICU_DATA", "/nonexistent-edge-icu"),))
        self.assertIn("/nonexistent-edge-icu", missing_icu.expected_stderr_contains)
        # Guest ICU still decodes Shift_JIS after falling back to the mount.
        self.assertEqual(missing_icu.expected_stdout, "12354\n")  # U+3042

    def test_build_cases_skip_missing_icu_check_for_embedded_builds(self):
        module = load_script()

        cases = module.build_cases("example.com", include_network=False, external_static_data=False)

        names = [case.name for case in cases]
        self.assertNotIn(MISSING_ICU_CASE_NAME, names)
        for name in STATIC_DATA_CASE_NAMES:
            self.assertIn(name, names)

    def test_static_data_case_scripts_are_ascii(self):
        # Keep subprocess text decoding independent of the host locale.
        module = load_script()

        for case in module.build_cases("example.com", include_network=True):
            case.script.encode("ascii")
            case.expected_stdout.encode("ascii")

    def test_build_command_forwards_case_env(self):
        module = load_script()

        case = module.Case(name="env", script="0", env=(("EDGE_ICU_DATA", "/x"), ("A", "b=c")))
        cmd = module.build_command("wasmer", case)

        self.assertEqual(cmd[:5], ["wasmer", "run", ".", "--experimental-napi", "--net"])
        self.assertEqual(cmd[5:9], ["--env", "EDGE_ICU_DATA=/x", "--env", "A=b=c"])
        self.assertEqual(cmd[9:], ["--", "-e", "0"])

    def test_build_command_without_env(self):
        module = load_script()

        cmd = module.build_command("wasmer", module.Case(name="plain", script="1"))

        self.assertEqual(cmd, ["wasmer", "run", ".", "--experimental-napi", "--net", "--", "-e", "1"])


if __name__ == "__main__":
    unittest.main()
