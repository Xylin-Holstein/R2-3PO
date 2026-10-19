#!/usr/bin/env python3
"""Regression checks for complete cleanup after either startup thread fails."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "r2.c").read_text(encoding="utf-8")
WORKFLOW = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")


class StartupFailureUnwindTests(unittest.TestCase):
    def test_fault_injection_is_test_build_only(self):
        helper = re.search(
            r"static int r2_startup_thread_create\(.*?\n}\n",
            SOURCE,
            re.S,
        )
        self.assertIsNotNone(helper, "startup thread wrapper is missing")
        body = helper.group(0)
        self.assertIn("#ifdef R2_TEST_STARTUP_FAILURE_INJECTION", body)
        self.assertIn("R2_TEST_FAIL_HANDS_THREAD_CREATE", SOURCE)
        self.assertIn("R2_TEST_FAIL_DIARY_THREAD_CREATE", SOURCE)
        self.assertIn("return pthread_create(thread, NULL, start_routine, arg);", body)

    def test_both_thread_failures_return_to_main_cleanup(self):
        init = SOURCE[SOURCE.index("int r2_init(void)") : SOURCE.index("void r2_shutdown(void)")]
        self.assertIn(
            'r2_startup_thread_create(&hands_thread, hands_worker, NULL,\n'
            '                                    "R2_TEST_FAIL_HANDS_THREAD_CREATE") != 0',
            init,
        )
        self.assertIn(
            'r2_startup_thread_create(&diary_thread, autonomous_thinking, NULL,\n'
            '                                    "R2_TEST_FAIL_DIARY_THREAD_CREATE") != 0',
            init,
        )
        hands_failure = init.split("R2_TEST_FAIL_HANDS_THREAD_CREATE", 1)[1].split(
            "hands_thread_started = 1;", 1
        )[0]
        diary_failure = init.split("R2_TEST_FAIL_DIARY_THREAD_CREATE", 1)[1].split(
            "atomic_store(&diary_thread_started, 1);", 1
        )[0]
        for branch in (hands_failure, diary_failure):
            self.assertIn("return -1;", branch)
            self.assertNotIn("sqlite3_close(db)", branch)
            self.assertNotIn("r2_diary_shutdown()", branch)
            self.assertNotIn("curl_global_cleanup()", branch)
        self.assertIn("pthread_join(hands_thread, NULL);", diary_failure)

    def test_required_init_failures_return_through_central_cleanup(self):
        init = SOURCE[SOURCE.index("int r2_init(void)") : SOURCE.index("void r2_shutdown(void)")]
        shutdown = SOURCE[SOURCE.index("void r2_shutdown(void)") : SOURCE.index("int main(void)")]
        self.assertIn("#define R2_INIT_FAIL", SOURCE)
        self.assertNotRegex(init, r"\bdie\s*\(",
                            "r2_init must not exit around main's cleanup owner")
        self.assertIn("init_attempt_active = 1;", init)
        self.assertIn("if (!init_attempt_active && !core_initialized", shutdown)
        diary_failure = init.split("r2_diary_init() != 0", 1)[1].split(
            "diary_initialized = 1;", 1
        )[0]
        self.assertIn("r2_diary_shutdown();", diary_failure)
        self.assertIn("if (curl_global_initialized)", shutdown)
        self.assertIn("curl_global_initialized = 0;", shutdown)

    def test_main_unwinds_when_initialization_fails(self):
        main = SOURCE[SOURCE.index("int main(void)") :]
        failure = main.index("if (r2_init() != 0)")
        shutdown = main.index("r2_shutdown();", failure)
        failure_return = main.index("return 1;", failure)
        self.assertLess(shutdown, failure_return)

    def test_regression_is_wired_into_full_workflow(self):
        self.assertIn("python3 tests/startup_failure_unwind_smoke.py", WORKFLOW)
        self.assertIn("Compile R2 with the same sources and flags as the launcher", WORKFLOW)

    def test_runtime_fault_injection_is_wired_into_full_workflow(self):
        self.assertIn("tests/startup_failure_unwind_runtime.c", WORKFLOW)
        self.assertIn("-DR2_TEST_STARTUP_FAILURE_INJECTION", WORKFLOW)
        self.assertIn("/tmp/r2-startup-unwind-test hands", WORKFLOW)
        self.assertIn("/tmp/r2-startup-unwind-test diary", WORKFLOW)


if __name__ == "__main__":
    unittest.main(verbosity=2)
