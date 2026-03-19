"""Windows 通知工具 - 用于 Claude Code 完成任务后发送通知"""
import sys
from win11toast import notify


def send_notification(title: str = "Claude Code", message: str = "任务完成"):
    """发送 Windows 通知"""
    notify(title=title, body=message)


if __name__ == '__main__':
    title = sys.argv[1] if len(sys.argv) > 1 else "Claude Code"
    message = sys.argv[2] if len(sys.argv) > 2 else "任务完成"
    send_notification(title, message)
