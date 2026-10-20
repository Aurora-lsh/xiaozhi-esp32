import json
from pathlib import Path
import shutil
import unittest
from unittest.mock import patch

from mcp_server import medicine_store, medicine_tools
from mcp_server.tests.test_medicine_store import make_temp_dir


class MedicineToolTests(unittest.TestCase):
    def setUp(self):
        self.dir = make_temp_dir()
        (self.dir / "library.json").write_text(json.dumps({
            "medicines": [{
                "name": "感冒灵颗粒",
                "aliases": ["感冒灵"],
                "spec": "10g×9袋",
                "dose": "一次1袋，一日3次。",
                "cautions": "服药期间不得饮酒。",
            }]
        }, ensure_ascii=False), encoding="utf-8")
        config = dict(medicine_store.DEFAULT_CONFIG)
        config["data_dir"] = str(self.dir / "data")
        config["library_file"] = str(self.dir / "library.json")
        self.store = medicine_store.MedicineStore(config=config)
        patcher = patch.object(medicine_store, "get_store", lambda: self.store)
        patcher.start()
        self.addCleanup(patcher.stop)

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    # -- 识别工具 ----------------------------------------------------------
    def test_identify_uses_medicine_prompt_and_does_not_write(self):
        def fake_capture(prompt):
            self.assertIn(self.store.config["identify_prompt"], prompt)
            return "感冒灵 10g×9袋", 900

        with patch.object(medicine_tools, "_capture_for_medicine", fake_capture):
            result = medicine_tools.identify_medicine()

        self.assertTrue(result["success"])
        self.assertTrue(result["matched"])
        self.assertEqual(result["name"], "感冒灵颗粒")
        self.assertEqual(result["duration_ms"], 900)
        self.assertEqual(self.store.query_records()["count"], 0)
        self.assertFalse(self.store.archive_path.exists())

    def test_identify_prompt_comes_from_config(self):
        captured = {}

        def fake_capture(prompt):
            captured["prompt"] = prompt
            return "感冒灵", None

        with patch.object(medicine_tools, "_capture_for_medicine", fake_capture):
            medicine_tools.identify_medicine(hint="这是降压药")

        self.assertIn(self.store.config["identify_prompt"], captured["prompt"])
        self.assertIn("这是降压药", captured["prompt"])

    def test_identify_returns_failure_without_guessing_on_camera_error(self):
        def boom(prompt):
            raise RuntimeError("ESP32 请求失败或超时")

        with patch.object(medicine_tools, "_capture_for_medicine", boom):
            result = medicine_tools.identify_medicine()

        self.assertFalse(result["success"])
        self.assertIn("超时", result["error"])
        self.assertIn("重新拍摄", result["instruction"])

    def test_identify_rejects_empty_vision_message(self):
        with patch.object(medicine_tools, "_capture_for_medicine",
                          lambda prompt: ("   ", None)):
            result = medicine_tools.identify_medicine()

        self.assertFalse(result["success"])

    # -- 记录工具 ----------------------------------------------------------
    def test_remember_then_log_then_query_round_trip(self):
        remembered = medicine_tools.remember_medicine("感冒灵颗粒")
        logged = medicine_tools.log_medicine_taken("感冒灵颗粒", "1袋", "20:00")
        queried = medicine_tools.query_medicine_records(date="today")

        self.assertEqual(remembered["action"], "created")
        self.assertFalse(logged["duplicate"])
        self.assertEqual(queried["count"], 1)
        self.assertEqual(queried["records"][0]["time"], "20:00")

    def test_double_confirmation_is_reported_as_duplicate(self):
        medicine_tools.log_medicine_taken("感冒灵颗粒", "1袋", "20:00")
        again = medicine_tools.log_medicine_taken("感冒灵颗粒", "1袋", "20:00")

        self.assertTrue(again["duplicate"])
        self.assertEqual(self.store.query_records()["count"], 1)

    def test_blank_name_returns_readable_error(self):
        result = medicine_tools.log_medicine_taken("")

        self.assertFalse(result["success"])
        self.assertIn("药名", result["error"])

    def test_non_integer_limit_is_ignored(self):
        medicine_tools.log_medicine_taken("感冒灵颗粒", "1袋", "20:00")

        result = medicine_tools.query_medicine_records(limit="很多")

        self.assertTrue(result["success"])
        self.assertEqual(result["count"], 1)

    # -- 提醒工具 ----------------------------------------------------------
    def test_reminder_tools_explain_passive_limitation(self):
        created = medicine_tools.set_medicine_reminder("感冒灵颗粒", "08:00")

        self.assertTrue(created["success"])
        self.assertIn("不会", created["instruction"])
        self.assertIn("check_medicine_reminders", created["instruction"])

    def test_check_reminders_reports_due_and_asks_before_logging(self):
        medicine_tools.set_medicine_reminder("感冒灵颗粒", "08:00")
        today = self.store.now().strftime("%Y-%m-%d")

        result = medicine_tools.check_medicine_reminders("%s 08:05" % today)

        self.assertEqual(result["due_count"], 1)
        # 只是提醒，还没有写入任何服药记录。
        self.assertEqual(self.store.query_records()["count"], 0)

    # -- 资料工具 ----------------------------------------------------------
    def test_get_medicine_info_serves_library_facts(self):
        result = medicine_tools.get_medicine_info("感冒灵")

        self.assertTrue(result["found"])
        self.assertEqual(result["medicine"]["spec"], "10g×9袋")

    def test_remember_with_oral_usage_then_ask_frequency(self):
        """端到端：记住“一天三次”，下次问用量能答出来并标明来源。"""
        fact = medicine_tools.remember_medicine(
            "布洛芬混悬液", frequency="一日3次", dose="5ml")

        self.assertTrue(fact["success"])
        self.assertIn("口述", fact["instruction"])

        answer = medicine_tools.get_medicine_info("布洛芬混悬液")

        self.assertTrue(answer["found"])
        self.assertEqual(answer["medicine"]["frequency"], "一日3次")
        self.assertEqual(answer["source"], "用户口述")

    def test_log_and_remember_are_independent_for_frequency(self):
        """只记服药流水、没说方案时，不应凭空给出“一天几次”。"""
        medicine_tools.log_medicine_taken("布洛芬混悬液", "5ml", "08:00")

        answer = medicine_tools.get_medicine_info("布洛芬混悬液")

        self.assertFalse(answer["found"])
        self.assertIn("不要编造", answer["instruction"])

    def test_get_medicine_info_missing_medicine_is_honest(self):
        result = medicine_tools.get_medicine_info("阿司匹林")

        self.assertFalse(result["found"])
        self.assertIn("不要编造", result["instruction"])

    # -- 注册 --------------------------------------------------------------
    def test_all_medicine_tools_are_registered_on_the_server(self):
        from mcp_server.camera_mcp import create_server
        from mcp_server import camera_mcp

        create_server()

        # 通过工具函数本身验证注册来源，避免依赖 FastMCP 内部 API。
        names = {tool.__name__ for tool in camera_mcp.medicine_tools()}
        self.assertEqual(names, {
            "identify_medicine", "remember_medicine", "log_medicine_taken",
            "query_medicine_records", "set_medicine_reminder",
            "check_medicine_reminders", "get_medicine_info",
        })
        self.assertEqual(len(camera_mcp.medicine_tools()), 7)

    def test_medicine_tools_are_disabled_when_config_disables_them(self):
        config = dict(self.store.config, enabled=False)
        disabled = medicine_store.MedicineStore(config=config)

        with patch.object(medicine_store, "get_store", lambda: disabled):
            result = medicine_tools.identify_medicine()

        self.assertFalse(result["success"])
        self.assertIn("enabled", result["error"])


if __name__ == "__main__":
    unittest.main()
