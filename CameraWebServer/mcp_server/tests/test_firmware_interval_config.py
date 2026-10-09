import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class FirmwareIntervalConfigTests(unittest.TestCase):
    def test_interval_zero_is_allowed_at_every_configuration_layer(self):
        config = (ROOT / "vision_config.h").read_text(encoding="utf-8")
        httpd = (ROOT / "app_httpd.cpp").read_text(encoding="utf-8")
        vision = (ROOT / "vision.cpp").read_text(encoding="utf-8")

        self.assertRegex(config, r"#define\s+VISION_MIN_INTERVAL_MS\s+0UL\b")
        self.assertIn('id="inpInterval" min="0"', httpd)
        self.assertNotIn("if (!sec || sec < 5) sec = 5;", httpd)
        self.assertIn("if (isNaN(sec) || sec < 0) sec = 0;", httpd)

        # 0 is a valid configured interval, so the setter must not use it as
        # the old "leave unchanged" sentinel.
        setter = re.search(
            r"void visionSetAutoMode\(bool enabled, unsigned long intervalMs\)(.*?)\n}\n",
            vision,
            re.DOTALL,
        )
        self.assertIsNotNone(setter)
        self.assertNotIn("if (intervalMs > 0)", setter.group(1))
        self.assertIn("g_state.intervalMs = intervalMs;", setter.group(1))

        # Omitting the HTTP parameter must still preserve the current value,
        # while an explicit interval=0 must reach the setter unchanged.
        self.assertIn("unsigned long intervalMs = st.intervalMs;", httpd)


if __name__ == "__main__":
    unittest.main()
