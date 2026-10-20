import json
import os
from pathlib import Path
import shutil
import unittest
import uuid
from unittest.mock import patch

from mcp_server import medicine_store
from mcp_server.medicine_store import MedicineStore, MedicineStoreError, normalise_config

# 测试临时目录不放在系统 TMP：部分受限环境（含本项目开发机）会把 TMP 指到
# 不可写位置。这里用固定父目录 + 随机子目录名，而不是 tempfile.mkdtemp，
# 因为本机沙箱允许写入显式 mkdir 出来的目录，却拒绝对 mkdtemp 目录写入。
_TMP_ROOT = Path(__file__).resolve().parents[2] / ".test-tmp"


def make_temp_dir():
    _TMP_ROOT.mkdir(parents=True, exist_ok=True)
    path = _TMP_ROOT / ("t" + uuid.uuid4().hex[:12])
    path.mkdir(parents=True, exist_ok=True)
    return path


def _config(directory, **overrides):
    config = dict(medicine_store.DEFAULT_CONFIG)
    config["data_dir"] = str(Path(directory) / "data")
    config["library_file"] = str(Path(directory) / "library.json")
    config.update(overrides)
    return config


class MedicineStoreTests(unittest.TestCase):
    def setUp(self):
        self.dir = make_temp_dir()
        (self.dir / "library.json").write_text(json.dumps({
            "medicines": [{
                "name": "感冒灵颗粒",
                "aliases": ["感冒灵", "999感冒灵"],
                "spec": "10g×9袋",
                "usage": "开水冲服。",
                "dose": "一次1袋，一日3次。",
                "cautions": "服药期间不得饮酒。",
            }]
        }, ensure_ascii=False), encoding="utf-8")
        self.store = MedicineStore(config=_config(self.dir))

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    # -- 识别 --------------------------------------------------------------
    def test_identify_matches_library_without_writing_anything(self):
        result = self.store.resolve_identification("999感冒灵", duration_ms=1200)

        self.assertTrue(result["success"])
        self.assertTrue(result["matched"])
        self.assertEqual(result["name"], "感冒灵颗粒")
        self.assertEqual(result["spec"], "10g×9袋")
        self.assertEqual(result["duration_ms"], 1200)
        # 识别不落盘：既没有档案，也没有服药记录。
        self.assertFalse(self.store.archive_path.exists())
        self.assertFalse(self.store.log_path.exists())

    def test_identify_unknown_medicine_keeps_raw_and_flags_not_matched(self):
        result = self.store.resolve_identification("某某某片 0.5g")

        self.assertTrue(result["success"])
        self.assertFalse(result["matched"])
        self.assertEqual(result["raw"], "某某某片 0.5g")
        self.assertEqual(result["spec"], "")
        self.assertIn("不要编造", result["hint"])

    def test_identify_trims_long_text_to_configured_length(self):
        store = MedicineStore(config=_config(self.dir, identify_max_chars=6))
        result = store.resolve_identification("完全未知的药品名称很长很长")

        self.assertFalse(result["matched"])
        self.assertLessEqual(len(result["name"]), 6)

    def test_identify_empty_text_is_an_error(self):
        with self.assertRaises(MedicineStoreError):
            self.store.resolve_identification("   ")

    # -- 档案 --------------------------------------------------------------
    def test_remember_creates_then_updates_single_record(self):
        first = self.store.remember("感冒灵颗粒")
        second = self.store.remember("感冒灵颗粒")

        self.assertEqual(first["action"], "created")
        self.assertEqual(second["action"], "updated")
        self.assertEqual(second["total_medicines"], 1)
        # 库里有规格时自动补全，不用用户重复说。
        self.assertEqual(second["medicine"]["spec"], "10g×9袋")
        self.assertEqual(second["medicine"]["seen_count"], 2)

    def test_remember_requires_a_name(self):
        with self.assertRaises(MedicineStoreError):
            self.store.remember("  ")

    def test_archive_survives_a_new_store_instance(self):
        self.store.remember("感冒灵颗粒")

        reopened = MedicineStore(config=_config(self.dir))
        archive = reopened._load_archive()
        self.assertEqual(len(archive["medicines"]), 1)

    # -- 服药记录 ----------------------------------------------------------
    def test_log_creates_record_and_is_queryable(self):
        result = self.store.log_taken("感冒灵颗粒", "1袋", "20:00")

        self.assertTrue(result["success"])
        self.assertFalse(result["duplicate"])
        self.assertEqual(result["record"]["dose"], "1袋")
        self.assertTrue(result["record"]["taken_at"].endswith("20:00:00"))

        query = self.store.query_records(date="today")
        self.assertEqual(query["count"], 1)
        self.assertEqual(query["records"][0]["name"], "感冒灵颗粒")
        self.assertEqual(query["records"][0]["time"], "20:00")

    def test_repeated_log_within_window_is_deduplicated(self):
        first = self.store.log_taken("感冒灵颗粒", "1袋", "20:00")
        again = self.store.log_taken("感冒灵颗粒", "1袋", "20:00")

        self.assertFalse(first["duplicate"])
        self.assertTrue(again["duplicate"])
        self.assertEqual(self.store.query_records()["count"], 1)

    def test_different_dose_or_later_time_creates_a_new_record(self):
        self.store.log_taken("感冒灵颗粒", "1袋", "08:00")
        other_dose = self.store.log_taken("感冒灵颗粒", "2袋", "08:00")
        later = self.store.log_taken("感冒灵颗粒", "1袋", "20:00")

        self.assertFalse(other_dose["duplicate"])
        self.assertFalse(later["duplicate"])
        self.assertEqual(self.store.query_records()["count"], 3)

    def test_identify_only_never_creates_a_medication_record(self):
        self.store.remember("感冒灵颗粒")
        self.store.resolve_identification("感冒灵")

        query = self.store.query_records()
        self.assertEqual(query["count"], 0)
        self.assertIn("如实", query["instruction"])

    def test_query_filters_by_name_and_reports_truncation(self):
        for hour in ("08:00", "09:00", "10:00"):
            self.store.log_taken("感冒灵颗粒", "1袋", hour)
        self.store.log_taken("布洛芬缓释胶囊", "1粒", "08:00")

        limited = self.store.query_records(limit=2)
        self.assertEqual(limited["count"], 2)
        self.assertEqual(limited["matched_total"], 4)
        self.assertTrue(limited["truncated"])

        by_name = self.store.query_records(name="布洛芬")
        self.assertEqual(by_name["count"], 0)  # 未在库中建档，名称须完全对应
        self.assertEqual(self.store.query_records(name="布洛芬缓释胶囊")["count"], 1)

    def test_broken_log_line_is_skipped_without_losing_others(self):
        self.store.log_taken("感冒灵颗粒", "1袋", "08:00")
        with open(self.store.log_path, "a", encoding="utf-8") as stream:
            stream.write("{not json}\n")

        self.assertEqual(self.store.query_records()["count"], 1)

    def test_records_are_newest_first(self):
        self.store.log_taken("感冒灵颗粒", "1袋", "08:00")
        self.store.log_taken("感冒灵颗粒", "1袋", "20:00")

        records = self.store.query_records()["records"]
        self.assertEqual([item["time"] for item in records], ["20:00", "08:00"])

    # -- 提醒 --------------------------------------------------------------
    def test_reminder_is_not_due_before_its_time(self):
        self.store.set_reminder("感冒灵颗粒", "08:00")
        today = self.store.now().strftime("%Y-%m-%d")
        early = "%s 07:30" % today

        result = self.store.check_reminders(early)

        self.assertEqual(result["due_count"], 0)
        self.assertEqual(len(result["upcoming"]), 1)
        self.assertEqual(result["upcoming"][0]["in_minutes"], 30)

    def test_reminder_becomes_due_after_its_time(self):
        self.store.set_reminder("感冒灵颗粒", "08:00")
        today = self.store.now().strftime("%Y-%m-%d")

        result = self.store.check_reminders("%s 08:00" % today)

        self.assertEqual(result["due_count"], 1)
        self.assertEqual(result["due"][0]["name"], "感冒灵颗粒")
        self.assertIn("log_medicine_taken", result["instruction"])

    def test_reminder_disappears_once_medication_is_logged(self):
        self.store.set_reminder("感冒灵颗粒", "08:00")
        today = self.store.now().strftime("%Y-%m-%d")
        self.store.log_taken("感冒灵颗粒", "1袋", "%s 08:05" % today)

        result = self.store.check_reminders("%s 08:10" % today)

        self.assertEqual(result["due_count"], 0)
        self.assertEqual(result["taken_today"], ["感冒灵颗粒"])

    def test_later_dose_does_not_cancel_an_earlier_reminder(self):
        """晚上八点的服药不能抵消早上八点的提醒（回归用例）。"""
        self.store.set_reminder("感冒灵颗粒", "08:00")
        today = self.store.now().strftime("%Y-%m-%d")
        self.store.log_taken("感冒灵颗粒", "1袋", "%s 20:00" % today)

        result = self.store.check_reminders("%s 08:05" % today)

        self.assertEqual(result["due_count"], 1)
        self.assertEqual(result["due"][0]["name"], "感冒灵颗粒")

    def test_dose_before_reminder_does_not_cover_it(self):
        self.store.set_reminder("感冒灵颗粒", "08:00")
        today = self.store.now().strftime("%Y-%m-%d")
        self.store.log_taken("感冒灵颗粒", "1袋", "%s 06:00" % today)

        result = self.store.check_reminders("%s 08:05" % today)

        self.assertEqual(result["due_count"], 1)

    def test_missed_reminder_moves_to_missed_bucket(self):
        self.store.set_reminder("感冒灵颗粒", "08:00")
        today = self.store.now().strftime("%Y-%m-%d")

        result = self.store.check_reminders("%s 14:00" % today)

        self.assertEqual(result["due_count"], 0)
        self.assertEqual(len(result["missed"]), 1)
        self.assertEqual(result["missed"][0]["overdue_minutes"], 360)

    def test_disabled_reminder_is_ignored(self):
        self.store.set_reminder("感冒灵颗粒", "08:00")
        archive = self.store._load_archive()
        archive["reminders"][0]["enabled"] = False
        self.store._save_archive(archive)
        today = self.store.now().strftime("%Y-%m-%d")

        self.assertEqual(self.store.check_reminders("%s 08:30" % today)["due_count"], 0)

    def test_reminder_rejects_bad_time_text(self):
        with self.assertRaises(MedicineStoreError):
            self.store.set_reminder("感冒灵颗粒", "八点")
        with self.assertRaises(MedicineStoreError):
            self.store.set_reminder("感冒灵颗粒", "25:00")

    def test_reminder_same_time_is_updated_not_duplicated(self):
        self.store.set_reminder("感冒灵颗粒", "8:00")
        again = self.store.set_reminder("感冒灵颗粒", "08:00")

        self.assertEqual(again["action"], "updated")
        self.assertEqual(len(self.store._load_archive()["reminders"]), 1)
        self.assertEqual(again["reminder"]["at"], "08:00")

    # -- 资料 --------------------------------------------------------------
    def test_get_info_returns_library_fields(self):
        result = self.store.get_info("感冒灵")

        self.assertTrue(result["found"])
        self.assertEqual(result["medicine"]["name"], "感冒灵颗粒")
        self.assertIn("一日3次", result["medicine"]["dose"])

    def test_get_info_reports_missing_medicine_instead_of_inventing(self):
        result = self.store.get_info("不存在的药")

        self.assertTrue(result["success"])
        self.assertFalse(result["found"])
        self.assertIn("不要编造", result["instruction"])

    # -- 口述用药方案（药品库没有该药时）------------------------------------
    def test_oral_usage_is_answerable_after_remember(self):
        """用户口述“一天吃三次”，之后问用量必须能答出来。"""
        self.store.remember("布洛芬混悬液", frequency="一日3次", dose="5ml")

        result = self.store.get_info("布洛芬混悬液")

        self.assertTrue(result["found"])
        self.assertEqual(result["medicine"]["frequency"], "一日3次")
        self.assertEqual(result["medicine"]["dose"], "5ml")

    def test_oral_source_is_labelled_and_not_presented_as_manual(self):
        self.store.remember("布洛芬混悬液", frequency="一日3次")

        result = self.store.get_info("布洛芬混悬液")

        self.assertEqual(result["source"], "用户口述")
        self.assertIn("frequency", result["manual_fields"])
        self.assertIn("不要说得像药品说明书", result["instruction"])

    def test_library_entry_wins_over_oral_usage(self):
        """库内药优先用可靠资料，不被用户口述覆盖。"""
        self.store.remember("感冒灵颗粒", frequency="一日1次（用户说错）")

        result = self.store.get_info("感冒灵颗粒")

        self.assertEqual(result["source"], "本地药品库")
        self.assertIn("一日3次", result["medicine"]["dose"])
        self.assertNotEqual(result["medicine"].get("frequency"), "一日1次（用户说错）")

    def test_remember_without_oral_fields_still_reports_no_detail(self):
        self.store.remember("布洛芬混悬液")

        result = self.store.get_info("布洛芬混悬液")

        self.assertFalse(result["found"])
        self.assertEqual(result["known"]["name"], "布洛芬混悬液")

    def test_oral_fields_merge_and_track_across_repeated_remember(self):
        self.store.remember("布洛芬混悬液", frequency="一日3次")
        self.store.remember("布洛芬混悬液", cautions="饭后吃")

        result = self.store.get_info("布洛芬混悬液")

        self.assertEqual(result["medicine"]["frequency"], "一日3次")
        self.assertEqual(result["medicine"]["cautions"], "饭后吃")
        self.assertEqual(sorted(result["manual_fields"]), ["cautions", "frequency"])

    def test_oral_fields_survive_reload(self):
        self.store.remember("布洛芬混悬液", frequency="一日3次")

        reopened = MedicineStore(config=_config(self.dir))
        result = reopened.get_info("布洛芬混悬液")

        self.assertTrue(result["found"])
        self.assertEqual(result["medicine"]["frequency"], "一日3次")
        self.assertEqual(result["source"], "用户口述")

    # -- 配置 --------------------------------------------------------------
    def test_disabled_config_blocks_writes_but_not_reads(self):
        store = MedicineStore(config=_config(self.dir, enabled=False))

        with self.assertRaises(MedicineStoreError):
            store.log_taken("感冒灵颗粒")
        with self.assertRaises(MedicineStoreError):
            store.set_reminder("感冒灵颗粒", "08:00")

    def test_invalid_config_values_fall_back_to_defaults(self):
        config = normalise_config({
            "timezone_offset_minutes": "abc",
            "duplicate_window_seconds": 999999,
            "data_dir": "",
        })

        self.assertEqual(config["timezone_offset_minutes"], 480)
        self.assertEqual(config["duplicate_window_seconds"], 3600)
        self.assertEqual(config["data_dir"], medicine_store.DEFAULT_CONFIG["data_dir"])

    def test_missing_config_file_uses_defaults(self):
        store = MedicineStore(config_file=self.dir / "nope.json")

        self.assertTrue(store.config["enabled"])
        self.assertEqual(store.config["default_dose"], "1")

    def test_broken_archive_file_is_reported_not_silently_reset(self):
        self.store.data_dir.mkdir(parents=True, exist_ok=True)
        self.store.archive_path.write_text("{broken", encoding="utf-8")

        with self.assertRaises(MedicineStoreError):
            self.store.remember("感冒灵颗粒")


class MedicineConfigFileTests(unittest.TestCase):
    def test_example_config_is_valid_and_self_consistent(self):
        example = Path(__file__).resolve().parents[1] / "medicine_config.example.json"
        payload = json.loads(example.read_text(encoding="utf-8"))

        config = normalise_config(payload)
        for key in payload:
            if key.startswith("_"):
                continue
            self.assertIn(key, config, "示例配置里的 %s 没有被识别" % key)

    def test_shipped_library_loads_and_has_required_fields(self):
        store = MedicineStore(config=dict(
            medicine_store.DEFAULT_CONFIG,
            data_dir=str(make_temp_dir() / "d"),
        ))
        medicines = store.library()

        self.assertGreater(len(medicines), 0)
        for item in medicines:
            self.assertTrue(item.get("name"))
            self.assertIn("spec", item)
            self.assertIn("cautions", item)

    def test_get_store_reloads_when_config_file_changes(self):
        directory = make_temp_dir()
        try:
            config_file = directory / "medicine_config.json"
            config_file.write_text(json.dumps({"default_dose": "1"}), encoding="utf-8")
            with patch.dict("os.environ", {"MEDICINE_CONFIG_FILE": str(config_file)}):
                medicine_store.reset_store()
                try:
                    self.assertEqual(medicine_store.get_store().config["default_dose"], "1")

                    config_file.write_text(
                        json.dumps({"default_dose": "2"}), encoding="utf-8")
                    # 有些文件系统 mtime 精度较粗，主动推进一毫秒确保触发重载。
                    stat = config_file.stat()
                    os.utime(config_file, ns=(stat.st_atime_ns, stat.st_mtime_ns + 1_000_000))
                    self.assertEqual(medicine_store.get_store().config["default_dose"], "2")
                finally:
                    medicine_store.reset_store()
        finally:
            shutil.rmtree(directory, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
