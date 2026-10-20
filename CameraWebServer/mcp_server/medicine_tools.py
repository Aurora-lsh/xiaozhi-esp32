"""Medicine archive / medication log / reminders exposed as MCP tools.

These tools wrap :mod:`medicine_store`. They are deliberately thin: every
decision that matters (what counts as a duplicate, what is "due", what the
model is allowed to claim) lives in the store, so it can be unit-tested
without a camera, a robot, or the cloud model.

Boundary that must not be blurred:

* ``identify_medicine`` only reads the camera and never writes a record;
* ``log_medicine_taken`` is the only tool that creates a medication record;
* ``set_medicine_reminder`` only stores a schedule - MCP tools are passive and
  cannot wake the model, so reminders are read back through
  ``check_medicine_reminders``.
"""

from __future__ import annotations

import importlib
import logging

try:
    from . import medicine_store
    from .medicine_store import MedicineStoreError
except ImportError:  # running as a plain script from mcp_server/
    import medicine_store
    from medicine_store import MedicineStoreError

LOGGER = logging.getLogger("MEDICINE_TOOLS")


def get_store():
    """按名字转发到 medicine_store.get_store。

    这里必须每次通过模块属性取，而不是 ``from medicine_store import
    get_store``：后者会在导入时绑定函数对象，导致测试里替换
    ``medicine_store.get_store`` 失效，工具会一直用真实配置目录。
    """
    return medicine_store.get_store()


def _camera_module():
    """延迟导入 camera_mcp。

    camera_mcp 在导入期就会调用 medicine_tools()，所以这里不能在模块级
    导入它，否则形成循环导入。
    """
    if "." in __name__:
        package = __name__.rsplit(".", 1)[0]
        return importlib.import_module(package + ".camera_mcp")
    return importlib.import_module("camera_mcp")


def _failure(error):
    message = str(error)[:400] or "未知错误"
    LOGGER.warning("medicine tool failed: %s", message)
    return {
        "success": False,
        "error": message,
    }


def _capture_for_medicine(prompt):
    """Run one vision capture with the medicine-specific question."""
    client = _camera_module()._CLIENT
    result = client.trigger_wait(prompt=prompt, source="mcp")
    message = result.get("message")
    if not isinstance(message, str):
        raise MedicineStoreError("识别服务没有返回文本结果")
    return message, result.get("durationMs")


def identify_medicine(hint: str = "") -> dict:
    """拍摄药盒并识别药品名称与规格，只用于建档前的确认。

    调用时机：用户把药盒放到镜头前、问“这是什么药”时。
    本工具只识别，不写药品档案，也不写服药记录。

    hint: 可选，用户补充的线索（例如“这是降压药”），仅用于提示，不改变识别结果。
    """
    try:
        store = get_store()
    except MedicineStoreError as error:
        return _failure(error)
    if not store.config.get("enabled", True):
        return _failure("药品功能已在 medicine_config.json 中关闭（enabled=false）")

    prompt = store.config["identify_prompt"]
    if isinstance(hint, str) and hint.strip():
        prompt = prompt + "线索：" + hint.strip()[:40]
    try:
        message, duration_ms = _capture_for_medicine(prompt)
        return store.resolve_identification(message, duration_ms)
    except MedicineStoreError as error:
        return _failure(error)
    except Exception as error:  # camera / network / vision failures
        LOGGER.warning("identify_medicine capture failed: %s", error)
        return {
            "success": False,
            "error": str(error)[:400],
            "instruction": "识别失败时不要猜测药名，请让用户把药盒放正后重新拍摄。",
        }


def remember_medicine(name: str, spec: str = "", note: str = "",
                      usage: str = "", dose: str = "", frequency: str = "",
                      cautions: str = "") -> dict:
    """把药品记入“家里有哪些药”的档案。

    调用时机：用户明确说“记一下”“帮我记住”之后。
    同一药名与规格重复调用只更新档案，不会产生第二条记录。
    注意：建档不代表用户吃过药。

    如果用户同时口述了用药方法，一并传进来，这样以后问“一天吃几次”能答出来：
    - usage: 用法，例如“开水冲服”
    - dose: 单次剂量，例如“1袋”
    - frequency: 频次，例如“一日3次”
    - cautions: 用户提到的注意事项

    这些字段会被标记为用户口述，回答时必须说明来源，不能讲成说明书内容。

    name: 药品名称，必填。
    spec: 规格，例如“10g×9袋”。
    note: 备注，例如“放在客厅抽屉”。
    """
    try:
        return get_store().remember(name, spec, note, usage, dose, frequency, cautions)
    except MedicineStoreError as error:
        return _failure(error)
    except Exception as error:
        return _failure(error)


def log_medicine_taken(name: str, dose: str = "", taken_at: str = "") -> dict:
    """记录一次“已服药”。

    调用时机：仅当用户明确说出已经吃过药时调用。
    看到药盒、识别成功、提醒到点都不会自动产生服药记录。
    同名同剂量在配置的重复时间窗内再次提交会返回 duplicate=true，不新增记录。

    name: 药品名称，必填。
    dose: 剂量，例如“1袋”“2粒”，留空则使用配置的默认剂量。
    taken_at: 服药时间，留空表示现在；支持“20:00”或“2026-10-19 20:00”。
    """
    try:
        return get_store().log_taken(name, dose, taken_at)
    except MedicineStoreError as error:
        return _failure(error)
    except Exception as error:
        return _failure(error)


def query_medicine_records(date: str = "", name: str = "", limit: int = 0) -> dict:
    """查询服药记录，用于回答“今天吃过什么药”。

    返回按时间倒序的记录摘要，count 是实际返回条数，matched_total 是全部符合条件条数。
    没有记录时如实返回空列表，不要编造。

    date: 留空表示不限日期；也可用“today”“昨天”或“2026-10-19”。
    name: 只看某种药的记录，留空表示全部。
    limit: 最多返回多少条，0 表示使用配置里的默认值。
    """
    try:
        store = get_store()
        if not isinstance(limit, int):
            limit = 0
        return store.query_records(date, name, limit)
    except MedicineStoreError as error:
        return _failure(error)
    except Exception as error:
        return _failure(error)


def set_medicine_reminder(name: str, at: str, repeat_daily: bool = True) -> dict:
    """保存一条用药提醒计划。

    重要：本工具只保存计划，不会让机器人在到点后主动说话。
    MCP 工具是被动调用的，提醒需要在对话中通过 check_medicine_reminders 查询。

    name: 药品名称，必填。
    at: 提醒时间，24 小时制的“HH:MM”，例如“08:00”。
    repeat_daily: 是否每天重复。
    """
    try:
        return get_store().set_reminder(name, at, repeat_daily)
    except MedicineStoreError as error:
        return _failure(error)
    except Exception as error:
        return _failure(error)


def check_medicine_reminders(now: str = "") -> dict:
    """查询当前到点或即将到点的用药提醒。

    建议在用户询问“我该吃药了吗”“今天还有什么药要吃”时先调用一次。
    返回 due（到点且当天未记录服药）、missed（已过时间且当天未记录）、
    upcoming（即将到点）三类，以及 taken_today（今天已记录的药名）。
    只有 due/missed 里的药才需要向用户确认是否已服药，确认后才调用 log_medicine_taken。

    now: 留空表示当前时间；演示时可传入“2026-10-19 08:00”模拟时间。
    """
    try:
        return get_store().check_reminders(now)
    except MedicineStoreError as error:
        return _failure(error)
    except Exception as error:
        return _failure(error)


def get_medicine_info(name: str, spec: str = "") -> dict:
    """查询某种药的用法、剂量和注意事项。

    用于“语音讲解”和“用药问答”。查询顺序是：
    1. 本地药品库（人工维护，最可靠，source=本地药品库）；
    2. 用户此前口述并记进档案的用药方法（source=用户口述）；
    3. 都没有则 found=false。

    当 source 是“用户口述”时，回答必须说明这是用户告诉你的，
    不能说成说明书内容。found=false 时必须如实说资料不足，不要编造。

    name: 药品名称，必填。
    spec: 规格，可选，用于区分同名不同规格的药。
    """
    try:
        return get_store().get_info(name, spec)
    except MedicineStoreError as error:
        return _failure(error)
    except Exception as error:
        return _failure(error)


def medicine_tools():
    """返回需要注册到 MCP server 的药品工具列表。"""
    return [
        identify_medicine,
        remember_medicine,
        log_medicine_taken,
        query_medicine_records,
        set_medicine_reminder,
        check_medicine_reminders,
        get_medicine_info,
    ]
