"""Local, file-backed medicine archive, medication log and reminders.

设计要点
--------
1. 所有状态都落在 ``medicine_config.json`` 指定的 ``data_dir`` 下（默认
   ``runtime/medicine``，``runtime/`` 已在 .gitignore 中）。把整个
   ``mcp_server`` 目录拷到另一台电脑、或只改配置里的 ``data_dir`` 指向新路径，
   即可带着历史记录迁移。
2. 三种记录严格分开，互不自动生成：
   - 药品档案：家里有哪些药（``remember_medicine``）；
   - 服药记录：什么时候吃过（``log_medicine_taken``，唯一写入入口）；
   - 提醒计划：几点该吃（``set_medicine_reminder``，只存不推送）。
3. MCP 工具是被动调用的，本模块不会主动唤醒模型。定时能力由
   ``check_medicine_reminders`` 以“下拉查询”的方式模拟。
4. 进程内用一把 RLock 保护读改写，写入使用临时文件 + os.replace 原子替换。
   桥接是单进程，所以不做跨进程文件锁。
"""

from __future__ import annotations

import json
import logging
import os
from pathlib import Path
import re
import tempfile
import threading
from datetime import datetime, timedelta, timezone

BASE_DIR = Path(__file__).resolve().parent
CONFIG_FILENAME = "medicine_config.json"
LOGGER = logging.getLogger("MEDICINE_STORE")

DEFAULT_CONFIG = {
    "enabled": True,
    "data_dir": "runtime/medicine",
    "library_file": "medicine_library.json",
    "timezone_offset_minutes": 480,
    "identify_prompt": "只回答画面中药盒上的药品名称和规格，最多15个字，不要解释。",
    "identify_max_chars": 15,
    "duplicate_window_seconds": 90,
    "default_dose": "1",
    "query_limit": 20,
    "query_limit_max": 100,
    "reminder_grace_minutes": 120,
}

_NAME_MAX = 60
_DOSE_MAX = 40
_NOTE_MAX = 200

# 口述用药方案的字段中文名，用于报错与来源说明。
_FIELD_LABELS = {
    "usage": "用法",
    "dose": "剂量",
    "frequency": "频次",
    "cautions": "注意事项",
}

_CONFIG_LOCK = threading.Lock()
_STORE = None
_STORE_SIGNATURE = None


class MedicineStoreError(RuntimeError):
    """可读的药品功能错误，工具层会把它转成 success=false。"""


# ---------------------------------------------------------------------------
# 配置
# ---------------------------------------------------------------------------

def config_path() -> Path:
    """配置文件路径：环境变量 MEDICINE_CONFIG_FILE 优先，否则取本目录。"""
    override = os.getenv("MEDICINE_CONFIG_FILE", "").strip()
    if override:
        return Path(override).expanduser()
    return BASE_DIR / CONFIG_FILENAME


def _coerce_int(value, fallback, minimum, maximum):
    try:
        number = int(value)
    except (TypeError, ValueError):
        return fallback
    return max(minimum, min(maximum, number))


def normalise_config(raw):
    """把配置文件内容合成为完整且已校验的配置。"""
    if raw is None:
        raw = {}
    if not isinstance(raw, dict):
        raise MedicineStoreError("medicine_config.json 的内容必须是 JSON 对象")
    config = dict(DEFAULT_CONFIG)
    config.update({key: value for key, value in raw.items() if key in DEFAULT_CONFIG})

    if not isinstance(config["enabled"], bool):
        config["enabled"] = DEFAULT_CONFIG["enabled"]
    for key in ("data_dir", "library_file", "identify_prompt"):
        if not isinstance(config[key], str) or not config[key].strip():
            config[key] = DEFAULT_CONFIG[key]
    config["timezone_offset_minutes"] = _coerce_int(
        config["timezone_offset_minutes"], 480, -720, 840)
    config["identify_max_chars"] = _coerce_int(
        config["identify_max_chars"], 15, 4, 200)
    config["duplicate_window_seconds"] = _coerce_int(
        config["duplicate_window_seconds"], 90, 0, 3600)
    config["query_limit"] = _coerce_int(config["query_limit"], 20, 1, 100)
    config["query_limit_max"] = _coerce_int(config["query_limit_max"], 100, 1, 500)
    config["reminder_grace_minutes"] = _coerce_int(
        config["reminder_grace_minutes"], 120, 0, 1440)
    if not isinstance(config["default_dose"], str) or not config["default_dose"]:
        config["default_dose"] = DEFAULT_CONFIG["default_dose"]
    return config


def _read_json(path, default, what):
    try:
        text = Path(path).read_text(encoding="utf-8-sig")
    except FileNotFoundError:
        return default
    except OSError as error:
        raise MedicineStoreError("读取%s失败：%s" % (what, error)) from error
    if not text.strip():
        return default
    try:
        return json.loads(text)
    except json.JSONDecodeError as error:
        raise MedicineStoreError("%s不是合法 JSON（%s）" % (what, error)) from error


def get_store():
    """取全局 MedicineStore；配置文件被改动后自动重建，无需重启桥接。"""
    global _STORE, _STORE_SIGNATURE
    path = config_path()
    try:
        signature = (str(path), path.stat().st_mtime_ns)
    except OSError:
        signature = (str(path), None)
    with _CONFIG_LOCK:
        if _STORE is None or signature != _STORE_SIGNATURE:
            _STORE = MedicineStore(path)
            _STORE_SIGNATURE = signature
        return _STORE


def reset_store():
    """测试用：丢弃缓存，下次 get_store() 重新加载配置。"""
    global _STORE, _STORE_SIGNATURE
    with _CONFIG_LOCK:
        _STORE = None
        _STORE_SIGNATURE = None


# ---------------------------------------------------------------------------
# 文本与时间工具
# ---------------------------------------------------------------------------

_PUNCT_PATTERN = re.compile(
    r"[\s\u3000,，.。;；:：!！?？、\"'“”‘’()（）\[\]【】<>《》/\\|~\-_+=*#@]+")


def normalise_text(value):
    """去掉空白与中英文标点，转小写，用于名称比对。"""
    return _PUNCT_PATTERN.sub("", str(value or "")).lower()


def _clean_display(value, limit):
    """保留可显示文本，去掉首尾标点并截断。"""
    text = str(value or "").strip()
    text = text.strip("。.,，;；:：!！?？、\"'“”‘’()（）[]【】 \t\r\n")
    if len(text) > limit:
        text = text[:limit]
    return text


def _require_text(value, field, limit, required=True):
    if value is None:
        value = ""
    if not isinstance(value, str):
        raise MedicineStoreError("%s 必须是字符串" % field)
    text = value.strip()
    if required and not text:
        raise MedicineStoreError("%s 不能为空" % field)
    if len(text) > limit:
        raise MedicineStoreError("%s 超过 %d 个字符" % (field, limit))
    return text


# ---------------------------------------------------------------------------
# 存储
# ---------------------------------------------------------------------------

class MedicineStore:
    def __init__(self, config_file=None, config=None):
        self.config_file = Path(config_file) if config_file else config_path()
        if config is None:
            raw = _read_json(self.config_file, {}, "药品配置文件 %s" % self.config_file)
            config = normalise_config(raw)
        self.config = normalise_config(config)
        self.lock = threading.RLock()

        self.data_dir = self._resolve(self.config["data_dir"])
        self.archive_path = self.data_dir / "medicines.json"
        self.log_path = self.data_dir / "medication_logs.jsonl"
        self.library_path = self._resolve(self.config["library_file"])

    # -- 路径 --------------------------------------------------------------
    @staticmethod
    def _resolve(value):
        path = Path(value).expanduser()
        if not path.is_absolute():
            path = BASE_DIR / path
        return path

    def _ensure_dir(self):
        self.data_dir.mkdir(parents=True, exist_ok=True)

    def _atomic_write(self, path, payload):
        self._ensure_dir()
        path = Path(path)
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(
                    mode="w", encoding="utf-8", dir=str(path.parent),
                    delete=False, suffix=".tmp") as stream:
                temporary = Path(stream.name)
                json.dump(payload, stream, ensure_ascii=False, indent=2)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, path)
        except OSError as error:
            raise MedicineStoreError("写入 %s 失败：%s" % (path.name, error)) from error
        finally:
            if temporary is not None and temporary.exists():
                temporary.unlink(missing_ok=True)

    def _load_archive(self):
        payload = _read_json(self.archive_path, None, "药品档案 medicines.json")
        if payload is None:
            return {"version": 1, "medicines": [], "reminders": []}
        if not isinstance(payload, dict):
            raise MedicineStoreError("medicines.json 格式错误：顶层必须是对象")
        medicines = payload.get("medicines")
        reminders = payload.get("reminders")
        if not isinstance(medicines, list) or not isinstance(reminders, list):
            raise MedicineStoreError("medicines.json 格式错误：缺少 medicines/reminders 数组")
        payload["version"] = payload.get("version", 1)
        return payload

    def _save_archive(self, archive):
        self._atomic_write(self.archive_path, archive)

    def _append_log(self, record):
        self._ensure_dir()
        line = json.dumps(record, ensure_ascii=False) + "\n"
        try:
            with open(self.log_path, "a", encoding="utf-8", newline="\n") as stream:
                stream.write(line)
                stream.flush()
                os.fsync(stream.fileno())
        except OSError as error:
            raise MedicineStoreError("写入服药记录失败：%s" % error) from error

    def _load_logs(self):
        try:
            text = self.log_path.read_text(encoding="utf-8")
        except FileNotFoundError:
            return []
        except OSError as error:
            raise MedicineStoreError("读取服药记录失败：%s" % error) from error
        records = []
        broken = 0
        for line in text.splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                item = json.loads(line)
            except json.JSONDecodeError:
                broken += 1
                continue
            if isinstance(item, dict):
                records.append(item)
            else:
                broken += 1
        if broken:
            LOGGER.warning("服药记录中有 %d 行无法解析，已跳过", broken)
        return records

    # -- 时间 --------------------------------------------------------------
    def _tz(self):
        return timezone(timedelta(minutes=self.config["timezone_offset_minutes"]))

    def now(self):
        return datetime.now(self._tz()).replace(tzinfo=None, microsecond=0)

    def parse_moment(self, value):
        """把外部传入的时间解析成本地 naive datetime。

        支持 ""（现在）、"HH:MM"、"HH:MM:SS"、"YYYY-MM-DD HH:MM"、
        以及带 T 或时区的 ISO 8601。
        """
        if value is None:
            return self.now()
        text = str(value).strip()
        if not text:
            return self.now()
        today = self.now().date()
        if re.fullmatch(r"\d{1,2}:\d{2}(:\d{2})?", text):
            moment = datetime.strptime(
                text if text.count(":") == 2 else text + ":00", "%H:%M:%S")
            return datetime.combine(today, moment.time())
        candidate = text.replace("Z", "+00:00")
        for parser in (datetime.fromisoformat,):
            try:
                parsed = parser(candidate)
            except ValueError:
                continue
            if parsed.tzinfo is not None:
                parsed = parsed.astimezone(self._tz()).replace(tzinfo=None)
            return parsed.replace(microsecond=0)
        for fmt in ("%Y-%m-%d %H:%M", "%Y/%m/%d %H:%M", "%Y-%m-%d"):
            try:
                return datetime.strptime(text, fmt)
            except ValueError:
                continue
        raise MedicineStoreError(
            "时间格式无法识别：%s（示例：08:00 或 2026-10-19 20:00）" % text)

    @staticmethod
    def _stamp(moment):
        return moment.strftime("%Y-%m-%d %H:%M:%S")

    @staticmethod
    def _clock(moment):
        return moment.strftime("%H:%M")

    # -- 药品库 ------------------------------------------------------------
    def library(self):
        payload = _read_json(self.library_path, {"medicines": []},
                             "药品库 %s" % self.library_path)
        if isinstance(payload, dict):
            items = payload.get("medicines", [])
        elif isinstance(payload, list):
            items = payload
        else:
            raise MedicineStoreError("药品库格式错误：应为对象或数组")
        if not isinstance(items, list):
            raise MedicineStoreError("药品库格式错误：medicines 必须是数组")
        return [item for item in items if isinstance(item, dict) and item.get("name")]

    def match_library(self, text):
        """在药品库里找与识别文本最匹配的一条。"""
        target = normalise_text(text)
        if not target:
            return None
        best = None
        best_score = 0
        for item in self.library():
            names = [item.get("name", "")] + list(item.get("aliases") or [])
            for candidate in names:
                key = normalise_text(candidate)
                if not key:
                    continue
                if key in target or target in key:
                    if len(key) > best_score:
                        best, best_score = item, len(key)
        return best

    def resolve_identification(self, raw, duration_ms=None):
        """把视觉模型的一段短文本变成结构化药品信息。不写任何记录。"""
        text = _clean_display(raw, 400)
        if not text:
            raise MedicineStoreError("识别返回了空文本，请把药盒放正后重新拍摄")
        matched = self.match_library(text)
        max_chars = self.config["identify_max_chars"]
        if matched:
            name = str(matched.get("name")).strip()
            spec = str(matched.get("spec") or "").strip()
            result = {
                "success": True,
                "matched": True,
                "name": name,
                "spec": spec,
                "raw": text,
                "in_library": True,
                "hint": "已与本地药品库匹配，可直接调用 remember_medicine 建档。",
            }
            for key in ("usage", "dose", "cautions"):
                if matched.get(key):
                    result[key] = str(matched[key])
        else:
            name = _clean_display(text, max_chars)
            result = {
                "success": True,
                "matched": False,
                "name": name,
                "spec": "",
                "raw": text,
                "in_library": False,
                "hint": "本地药品库没有该药。请向用户确认药名，不要编造规格、用法或注意事项。",
            }
        if duration_ms is not None:
            result["duration_ms"] = duration_ms
        return result

    def get_info(self, name, spec=""):
        """提供用法、剂量、注意事项。

        顺序：本地药品库 -> 药品档案里用户口述的内容 -> 明确说没有。
        药品库优先级最高，因为它是人工维护的可靠资料；口述内容必须标注来源，
        不能让模型把它讲得像说明书。
        """
        self._require_enabled()
        requested = _require_text(name, "药名", _NAME_MAX)

        matched = self.match_library(requested)
        if matched is not None:
            info = {
                "name": str(matched.get("name")).strip(),
                "spec": str(matched.get("spec") or "").strip(),
            }
            for key in ("usage", "dose", "cautions"):
                if matched.get(key):
                    info[key] = str(matched[key])
            return {
                "success": True,
                "found": True,
                "medicine": info,
                "source": "本地药品库",
                "instruction": "回答时以这些字段为准；没有的字段就说不知道，不要补充推测。",
            }

        archive_hit = self._find_medicine(self._load_archive(), requested, spec)
        if archive_hit is None:
            return {
                "success": True,
                "found": False,
                "name": requested,
                "instruction": ("没有这个药的任何资料。不要编造用法、剂量或注意事项，"
                                "请建议用户查看说明书或咨询医生/药师。"),
            }

        manual = {field: str(archive_hit[field]) for field in self._MANUAL_FIELDS
                  if archive_hit.get(field)}
        known = {key: archive_hit[key] for key in ("name", "spec")
                 if archive_hit.get(key)}
        if not manual:
            return {
                "success": True,
                "found": False,
                "name": archive_hit.get("name", requested),
                "known": known,
                "instruction": ("该药只在档案里出现过，没有用法或剂量资料。"
                                "不要编造用药信息，请建议用户查看说明书或咨询医生/药师。"),
            }

        return {
            "success": True,
            "found": True,
            "medicine": dict(known, **manual),
            "source": "用户口述",
            "manual_fields": sorted(manual),
            "instruction": (
                "这些用药信息是用户自己说的，不是说明书内容。回答时必须说明来源，"
                "例如“你之前告诉我是%s”；不要说得像药品说明书，也不要补充你没被告诉的内容。"
                % "、".join("%s%s" % (_FIELD_LABELS[f], manual[f]) for f in sorted(manual))
            ),
        }

    # -- 档案 --------------------------------------------------------------
    def _require_enabled(self):
        if not self.config.get("enabled", True):
            raise MedicineStoreError("药品功能已在 medicine_config.json 中关闭（enabled=false）")

    @staticmethod
    def _find_medicine(archive, name, spec=""):
        target = normalise_text(name)
        if not target:
            return None
        spec_key = normalise_text(spec)
        candidates = []
        for item in archive.get("medicines", []):
            if normalise_text(item.get("name")) == target:
                candidates.append(item)
        if not candidates:
            return None
        if spec_key:
            for item in candidates:
                if normalise_text(item.get("spec")) == spec_key:
                    return item
        return candidates[0]

    # 口述用药方案的字段：只会来自用户说的话，绝不会伪装成药品库内容。
    _MANUAL_FIELDS = ("usage", "dose", "frequency", "cautions")

    def remember(self, name, spec="", note="", usage="", dose="", frequency="", cautions=""):
        """建立或更新药品档案。

        usage/dose/frequency/cautions 是用户口述的用药方案。它们单独存放并标记
        ``manual_fields``，读取时来源会标成“用户口述”，不能冒充药品库。
        """
        self._require_enabled()
        clean_name = _require_text(name, "药名", _NAME_MAX)
        clean_spec = _require_text(spec, "规格", _NAME_MAX, required=False)
        clean_note = _require_text(note, "备注", _NOTE_MAX, required=False)
        manual = {}
        for field, value in (("usage", usage), ("dose", dose),
                             ("frequency", frequency), ("cautions", cautions)):
            text = _require_text(value, _FIELD_LABELS[field], _NOTE_MAX, required=False)
            if text:
                manual[field] = text

        library_hit = self.match_library(clean_name)
        if library_hit and not clean_spec:
            clean_spec = str(library_hit.get("spec") or "").strip()

        with self.lock:
            archive = self._load_archive()
            now = self.now()
            existing = self._find_medicine(archive, clean_name, clean_spec)
            if existing is not None:
                existing["last_seen"] = self._stamp(now)
                existing["seen_count"] = int(existing.get("seen_count", 1)) + 1
                if clean_note:
                    existing["note"] = clean_note
                if clean_spec and not existing.get("spec"):
                    existing["spec"] = clean_spec
                self._apply_manual(existing, manual)
                action = "updated"
                record = existing
            else:
                record = {
                    "id": os.urandom(4).hex(),
                    "name": clean_name,
                    "spec": clean_spec,
                    "note": clean_note,
                    "source": "library" if library_hit else "manual",
                    "first_seen": self._stamp(now),
                    "last_seen": self._stamp(now),
                    "seen_count": 1,
                }
                self._apply_manual(record, manual)
                archive["medicines"].append(record)
                action = "created"
            self._save_archive(archive)
        result = {
            "success": True,
            "action": action,
            "medicine": record,
            "total_medicines": len(archive["medicines"]),
        }
        if manual:
            result["instruction"] = (
                "已记住用户口述的用药信息。以后回答时要说这是用户告诉你的，"
                "不要说得像说明书内容。")
        return result

    def _apply_manual(self, record, manual):
        """合并口述字段，并记录哪些字段来自用户口述。"""
        if not manual:
            return
        for field, value in manual.items():
            record[field] = value
        manual_fields = set(record.get("manual_fields") or [])
        manual_fields.update(manual)
        record["manual_fields"] = sorted(manual_fields)
        record["manual_updated_at"] = self._stamp(self.now())

    # -- 服药记录 ----------------------------------------------------------
    def log_taken(self, name, dose="", taken_at="", source="voice", note=""):
        self._require_enabled()
        clean_name = _require_text(name, "药名", _NAME_MAX)
        clean_dose = _require_text(dose, "剂量", _DOSE_MAX, required=False) \
            or self.config["default_dose"]
        clean_note = _require_text(note, "备注", _NOTE_MAX, required=False)
        moment = self.parse_moment(taken_at)

        with self.lock:
            archive = self._load_archive()
            medicine = self._find_medicine(archive, clean_name)
            if medicine is None:
                library_hit = self.match_library(clean_name)
                medicine = {
                    "id": os.urandom(4).hex(),
                    "name": str(library_hit.get("name")).strip() if library_hit else clean_name,
                    "spec": str(library_hit.get("spec") or "").strip() if library_hit else "",
                    "note": "",
                    "source": "library" if library_hit else "log",
                    "first_seen": self._stamp(moment),
                    "last_seen": self._stamp(moment),
                    "seen_count": 1,
                }
                archive["medicines"].append(medicine)
                self._save_archive(archive)
            else:
                medicine["last_seen"] = self._stamp(moment)
                self._save_archive(archive)

            window = self.config["duplicate_window_seconds"]
            for item in self._load_logs():
                if normalise_text(item.get("name")) != normalise_text(medicine["name"]):
                    continue
                if normalise_text(item.get("dose")) != normalise_text(clean_dose):
                    continue
                try:
                    previous = datetime.strptime(item.get("taken_at", ""), "%Y-%m-%d %H:%M:%S")
                except (TypeError, ValueError):
                    continue
                if abs((moment - previous).total_seconds()) <= window:
                    return {
                        "success": True,
                        "duplicate": True,
                        "record": item,
                        "instruction": "这条服药记录已经存在，不要重复记录，直接向用户确认即可。",
                    }

            record = {
                "id": os.urandom(4).hex(),
                "medicine_id": medicine.get("id", ""),
                "name": medicine["name"],
                "spec": medicine.get("spec", ""),
                "dose": clean_dose,
                "taken_at": self._stamp(moment),
                "recorded_at": self._stamp(self.now()),
                "source": _require_text(source, "来源", 20, required=False) or "voice",
                "note": clean_note,
            }
            self._append_log(record)
        return {
            "success": True,
            "duplicate": False,
            "record": record,
            "instruction": "已记录。向用户复述药名、剂量和时间，确认无误即可。",
        }

    def query_records(self, date="", name="", limit=0):
        self._require_enabled()
        day = None
        if date and str(date).strip():
            text = str(date).strip()
            if text in ("today", "今天"):
                day = self.now().date()
            elif text in ("yesterday", "昨天"):
                day = self.now().date() - timedelta(days=1)
            else:
                day = self.parse_moment(text).date()
        name_key = normalise_text(name)
        maximum = self.config["query_limit_max"]
        wanted = limit if isinstance(limit, int) and limit > 0 else self.config["query_limit"]
        wanted = max(1, min(maximum, wanted))

        with self.lock:
            logs = self._load_logs()

        selected = []
        for item in logs:
            if name_key and normalise_text(item.get("name")) != name_key:
                continue
            if day is not None:
                try:
                    stamped = datetime.strptime(item.get("taken_at", ""), "%Y-%m-%d %H:%M:%S")
                except (TypeError, ValueError):
                    continue
                if stamped.date() != day:
                    continue
            selected.append(item)

        selected.sort(key=lambda item: item.get("taken_at", ""), reverse=True)
        trimmed = selected[:wanted]
        summary = []
        for item in trimmed:
            summary.append({
                "name": item.get("name", ""),
                "dose": item.get("dose", ""),
                "taken_at": item.get("taken_at", ""),
                "time": item.get("taken_at", "")[11:16],
            })
        result = {
            "success": True,
            "count": len(trimmed),
            "matched_total": len(selected),
            "truncated": len(selected) > len(trimmed),
            "records": summary,
        }
        if day is not None:
            result["date"] = day.isoformat()
        if trimmed:
            result["instruction"] = "按时间倒序播报这些记录，不要添加没有出现的药。"
        else:
            result["instruction"] = "没有符合条件的服药记录，如实告诉用户，不要编造。"
        return result

    # -- 提醒 --------------------------------------------------------------
    def set_reminder(self, name, at, repeat_daily=True):
        self._require_enabled()
        clean_name = _require_text(name, "药名", _NAME_MAX)
        clock = self._parse_clock(at)

        with self.lock:
            archive = self._load_archive()
            medicine = self._find_medicine(archive, clean_name)
            if medicine is None:
                library_hit = self.match_library(clean_name)
                medicine = {
                    "id": os.urandom(4).hex(),
                    "name": str(library_hit.get("name")).strip() if library_hit else clean_name,
                    "spec": str(library_hit.get("spec") or "").strip() if library_hit else "",
                    "note": "",
                    "source": "library" if library_hit else "reminder",
                    "first_seen": self._stamp(self.now()),
                    "last_seen": self._stamp(self.now()),
                    "seen_count": 1,
                }
                archive["medicines"].append(medicine)

            existing = None
            for item in archive.get("reminders", []):
                if (normalise_text(item.get("name")) == normalise_text(medicine["name"])
                        and item.get("at") == clock):
                    existing = item
                    break
            if existing is not None:
                existing["enabled"] = True
                existing["repeat_daily"] = bool(repeat_daily)
                reminder = existing
                action = "updated"
            else:
                reminder = {
                    "id": os.urandom(4).hex(),
                    "medicine_id": medicine.get("id", ""),
                    "name": medicine["name"],
                    "at": clock,
                    "repeat_daily": bool(repeat_daily),
                    "enabled": True,
                    "created_at": self._stamp(self.now()),
                }
                archive.setdefault("reminders", []).append(reminder)
                action = "created"
            self._save_archive(archive)
        return {
            "success": True,
            "action": action,
            "reminder": reminder,
            "instruction": ("提醒已保存，但提醒不会让机器人在到点后主动说话；"
                            "到需要提醒的场景由对话调用 check_medicine_reminders 查询。"),
        }

    @staticmethod
    def _parse_clock(value):
        text = str(value or "").strip()
        match = re.fullmatch(r"(\d{1,2})[:：](\d{2})", text)
        if not match:
            raise MedicineStoreError("提醒时间格式应类似 08:00，收到：%s" % (text or "(空)"))
        hour = int(match.group(1))
        minute = int(match.group(2))
        if not 0 <= hour <= 23 or not 0 <= minute <= 59:
            raise MedicineStoreError("提醒时间超出范围：%s" % text)
        return "%02d:%02d" % (hour, minute)

    def check_reminders(self, now=""):
        self._require_enabled()
        moment = self.parse_moment(now)
        today = moment.date()
        grace = timedelta(minutes=self.config["reminder_grace_minutes"])

        with self.lock:
            archive = self._load_archive()
            logs = self._load_logs()

        taken_today = set()
        taken_moments = []
        for item in logs:
            try:
                stamped = datetime.strptime(item.get("taken_at", ""), "%Y-%m-%d %H:%M:%S")
            except (TypeError, ValueError):
                continue
            if stamped.date() == today:
                taken_today.add(normalise_text(item.get("name")))
                taken_moments.append((normalise_text(item.get("name")), stamped))

        due, missed, upcoming = [], [], []
        for reminder in archive.get("reminders", []):
            if not reminder.get("enabled", True):
                continue
            clock = reminder.get("at", "")
            try:
                hour, minute = (int(part) for part in clock.split(":"))
            except (ValueError, AttributeError):
                continue
            scheduled = datetime.combine(today, datetime.min.time()).replace(
                hour=hour, minute=minute)
            item = {
                "name": reminder.get("name", ""),
                "at": clock,
                "scheduled": self._stamp(scheduled),
            }
            # 只有落在本次提醒“覆盖窗口”内的那次服药才算已覆盖本提醒：
            # 允许比提醒时间早 30 分钟（提前一点吃），并且不超过宽限时间。
            # 不能只看当天有没有同名记录：早上八点的提醒，不该被晚上八点的服药抵消。
            name_key = normalise_text(reminder.get("name"))
            covered = any(
                key == name_key and scheduled - timedelta(minutes=30) <= stamped <= scheduled + grace
                for key, stamped in taken_moments
            )
            if moment >= scheduled:
                if covered:
                    continue
                if moment - scheduled <= grace:
                    due.append(item)
                else:
                    item = dict(item, overdue_minutes=int(
                        (moment - scheduled).total_seconds() // 60))
                    missed.append(item)
            else:
                item["in_minutes"] = int((scheduled - moment).total_seconds() // 60)
                upcoming.append(item)

        due.sort(key=lambda entry: entry["at"])
        upcoming.sort(key=lambda entry: entry["at"])
        result = {
            "success": True,
            "now": self._stamp(moment),
            "due": due,
            "due_count": len(due),
            "missed": missed,
            "upcoming": upcoming[:3],
            "taken_today": sorted(taken_today),
        }
        if due:
            names = "、".join("%s（%s）" % (entry["name"], entry["at"]) for entry in due)
            result["instruction"] = (
                "有到点提醒：%s。请提醒用户服药；只有用户明确回答已经吃过，"
                "才调用 log_medicine_taken 记录。" % names)
        elif missed:
            names = "、".join("%s（%s）" % (entry["name"], entry["at"]) for entry in missed)
            result["instruction"] = (
                "这些提醒已经过了时间但当天没有服药记录：%s。请询问用户是否已经吃过，"
                "不要自行记录。" % names)
        else:
            result["instruction"] = "当前没有到点的用药提醒，如实告诉用户。"
        return result
