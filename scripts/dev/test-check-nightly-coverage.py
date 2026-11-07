# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression tests for the nightly module-coverage ratchet."""

import importlib.util
import os
import tempfile
import unittest

SCRIPT = os.path.join(os.path.dirname(__file__), "check-nightly-coverage.py")
SPEC = importlib.util.spec_from_file_location("nightly_coverage", SCRIPT)
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


class NightlyCoverageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.kernel = os.path.join(self.temp.name, "kernel")
        os.mkdir(self.kernel)
        for name in ("abi", "mm", "time"):
            os.mkdir(os.path.join(self.kernel, name))
        self.workflow = os.path.join(self.temp.name, "nightly.yml")

    def tearDown(self):
        self.temp.cleanup()

    def write_workflow(self, text):
        with open(self.workflow, "w", encoding="utf-8") as stream:
            stream.write(text)

    def test_missing_module_is_reported(self):
        self.write_workflow("jobs:\n  soak:\n    # module: mm\n")
        report = CHECKER.analyze(self.kernel, self.workflow, baseline=3)
        self.assertEqual(report["missing"], ["abi", "time"])

    def test_stale_declaration_fails_even_below_baseline(self):
        self.write_workflow("jobs:\n  soak:\n    # module: mm removed_module\n")
        report = CHECKER.analyze(self.kernel, self.workflow, baseline=3)
        self.assertEqual(report["stale"], ["removed_module"])
        self.assertFalse(report["ok"])

    def test_annotation_outside_job_does_not_count(self):
        self.write_workflow("# module: abi mm time\njobs:\n  soak:\n    steps: []\n")
        report = CHECKER.analyze(self.kernel, self.workflow, baseline=0)
        self.assertEqual(report["missing"], ["abi", "mm", "time"])

    def test_annotation_in_step_comment_does_not_count(self):
        self.write_workflow(
            "jobs:\n  soak:\n    steps:\n      - name: example\n"
            "        run: echo test\n        # module: abi\n")
        report = CHECKER.analyze(self.kernel, self.workflow, baseline=3)
        self.assertEqual(report["missing"], ["abi", "mm", "time"])

    def test_baseline_is_a_hard_upper_bound(self):
        self.write_workflow("jobs:\n  soak:\n    # module: mm\n")
        report = CHECKER.analyze(self.kernel, self.workflow, baseline=1)
        self.assertEqual(len(report["missing"]), 2)
        self.assertFalse(report["ok"])

    def test_coverage_at_baseline_passes(self):
        self.write_workflow("jobs:\n  soak:\n    # module: abi mm\n")
        report = CHECKER.analyze(self.kernel, self.workflow, baseline=1)
        self.assertEqual(report["missing"], ["time"])
        self.assertTrue(report["ok"])


if __name__ == "__main__":
    unittest.main()
