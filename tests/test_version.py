# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Raúl Milla (DominicZe)
"""The build ID must never end up as the 'nobuildid' placeholder in CI."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import generate_version  # noqa: E402


class FakeRun:
    """Stand-in for subprocess.run: returns a SHA or raises (no git)."""

    def __init__(self, output=None, error=None):
        self.output = output
        self.error = error
        self.calls = []

    def __call__(self, args, **kwargs):
        self.calls.append((args, kwargs))
        if self.error:
            raise self.error
        result = type('Result', (), {})()
        result.stdout = self.output
        return result


class VersionTests(unittest.TestCase):
    def test_git_short_sha(self):
        self.assertEqual(generate_version.build_id(FakeRun('fbc1234\n'), {},
                                                   warn=lambda *_: None), 'fbc1234')

    def test_ci_without_git_uses_the_event_sha(self):
        env = {'GITHUB_SHA': '0123456789abcdef0123456789abcdef01234567',
               'GITHUB_ACTIONS': 'true'}
        self.assertEqual(generate_version.build_id(
            FakeRun(error=FileNotFoundError()), env, warn=lambda *_: None), '0123456')

    def test_ci_without_any_source_is_an_error(self):
        with self.assertRaises(SystemExit):
            generate_version.build_id(FakeRun(error=FileNotFoundError()),
                                      {'GITHUB_ACTIONS': 'true'}, warn=lambda *_: None)

    def test_local_without_git_keeps_the_placeholder(self):
        self.assertEqual(generate_version.build_id(
            FakeRun(error=FileNotFoundError()), {}, warn=lambda *_: None), 'nobuildid')

    def test_rejects_an_overlong_sha(self):
        with self.assertRaises(SystemExit):
            generate_version.build_id(FakeRun('a' * 40), {}, warn=lambda *_: None)


if __name__ == '__main__':
    unittest.main()
