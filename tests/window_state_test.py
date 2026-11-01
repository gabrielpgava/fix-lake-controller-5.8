"""Synthetic hierarchy/Win32-query snapshots. No desktop/process inspected."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "window_state", Path(__file__).resolve().parents[1] / "scripts" / "Inspect-WindowState.py")
state = importlib.util.module_from_spec(spec)
spec.loader.exec_module(state)


class FakeWindows:
    def __init__(self, parent, ready=True, visible=True):
        self.parent = parent
        self.ready = ready
        self.visible = visible
        self.properties_read = []

    def GetWindowRect(self, hwnd, result):
        result._obj.left, result._obj.top = -1024, 40
        result._obj.right, result._obj.bottom = 0, 616
        return True

    def GetClientRect(self, hwnd, result):
        result._obj.left, result._obj.top = 0, 0
        result._obj.right, result._obj.bottom = 1088, 612
        return True

    def GetWindowLongW(self, hwnd, index):
        return 0x50000000

    def GetPropW(self, hwnd, name):
        self.properties_read.append(hwnd)
        return self.ready

    def GetParent(self, hwnd):
        return self.parent.get(hwnd, 0)

    def IsWindowVisible(self, hwnd):
        return self.visible

    def IsWindowEnabled(self, hwnd):
        return True


class WindowSnapshots(unittest.TestCase):
    def snapshot(self, host_class, ready=True):
        api = FakeWindows({2: 1}, ready)
        host = state.query_window(api, 1, 7, host_class)
        canvas = state.query_window(api, 2, 7, "Lake Controller")
        windows = state.add_relationships([host, canvas])
        self.assertEqual(api.properties_read, [1])
        self.assertEqual(canvas["host_setup_completed"], "NotApplicable")
        self.assertEqual(host["known_children"], ["0x2"])
        self.assertEqual(canvas["parent"], "0x1")
        self.assertNotIn("title", canvas)
        return windows

    def test_scaled_host_canvas(self):
        host, canvas = self.snapshot("LakeWindowedFix.ScaledHost.v1")
        self.assertEqual((host["role"], canvas["role"]), ("scaled-host", "canvas"))
        self.assertTrue(host["host_setup_completed"])
        self.assertEqual(host["rect"], [-1024, 40, 0, 616])

    def test_legacy_host(self):
        host, _ = self.snapshot("ContourParent")
        self.assertEqual(host["role"], "legacy-host")

    def test_incomplete_host_not_canvas_failure(self):
        host, _ = self.snapshot("LakeWindowedFix.ScaledHost.v1", False)
        self.assertFalse(host["host_setup_completed"])

    def test_unrelated_control_not_classified(self):
        self.assertIsNone(state.window_role("PRIVATE DEVICE TEXT"))


if __name__ == "__main__":
    unittest.main()
