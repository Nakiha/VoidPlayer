"""Windows 通知工具 - 用于 Claude Code 完成任务后发送通知

使用方式：
1. Claude 用 Write 工具写入 notify_content.json
2. 执行 python notify.py 发送通知（命令固定，可加入白名单）
"""
import json
from pathlib import Path
from win11toast import notify

CONTENT_FILE = Path(__file__).parent / "notify_content.json"


def send_notification(title: str = "Claude Code", message: str = "任务完成"):
    """发送 Windows 通知"""
    notify(title=title, body=message)


if __name__ == '__main__':
    if CONTENT_FILE.exists():
        data = json.loads(CONTENT_FILE.read_text(encoding="utf-8"))
        send_notification(data.get("title", "Claude Code"), data.get("message", "任务完成"))
        CONTENT_FILE.unlink()  # 发送后删除
