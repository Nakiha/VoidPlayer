"""
AutomationController - 自动化测试控制器

解析和执行 .vpmock 脚本，按照时间偏移触发动作
"""
import time
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Optional, Callable

from PySide6.QtCore import QObject, QTimer

from player.core.logging_config import get_logger

if TYPE_CHECKING:
    from player.core.action_dispatcher import ActionDispatcher


@dataclass
class MockCommand:
    """Mock 脚本命令"""
    time_offset: float       # 时间偏移 (秒)
    action_name: str         # 动作名称
    args: list               # 位置参数
    kwargs: dict             # 关键字参数
    line_number: int         # 行号 (用于错误报告)


class AutomationController(QObject):
    """
    自动化测试控制器

    功能:
    - 解析 .vpmock 脚本文件
    - 按照时间偏移调度动作执行
    - 记录执行日志和断言结果
    """

    def __init__(self, action_dispatcher: "ActionDispatcher", parent=None):
        super().__init__(parent)
        self._dispatcher = action_dispatcher
        self._commands: list[MockCommand] = []
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._on_tick)
        self._start_time: float = 0
        self._current_index: int = 0
        self._is_running: bool = False
        self._results: list[dict] = []
        self._on_finished: Optional[Callable] = None

    def load_script(self, path: str) -> bool:
        """
        加载 .vpmock 脚本文件

        Args:
            path: 脚本文件路径

        Returns:
            是否加载成功
        """
        try:
            script_path = Path(path)
            if not script_path.exists():
                get_logger().error(f"Mock script not found: {path}")
                return False

            self._commands = self._parse_script(script_path)
            get_logger().info(f"Loaded {len(self._commands)} commands from {path}")
            return True

        except Exception as e:
            get_logger().error(f"Failed to load mock script: {e}")
            return False

    def _parse_script(self, path: Path) -> list[MockCommand]:
        """解析 .vpmock 脚本"""
        commands = []

        with open(path, "r", encoding="utf-8") as f:
            for line_num, line in enumerate(f, start=1):
                line = line.strip()

                # 跳过空行和注释
                if not line or line.startswith("#"):
                    continue

                cmd = self._parse_line(line, line_num)
                if cmd:
                    commands.append(cmd)

        # 按时间偏移排序
        commands.sort(key=lambda c: c.time_offset)
        return commands

    def _parse_line(self, line: str, line_num: int) -> Optional[MockCommand]:
        """解析单行脚本"""
        import ast

        parts = line.split(",", 1)
        if len(parts) < 2:
            get_logger().warning(f"Invalid line {line_num}: {line}")
            return None

        try:
            time_offset = float(parts[0].strip())
        except ValueError:
            get_logger().warning(f"Invalid time offset at line {line_num}: {parts[0]}")
            return None

        # 解析动作和参数
        rest = parts[1].strip()

        # 格式: ACTION_NAME, arg1, arg2, ...
        # 或: ACTION_NAME
        action_parts = [p.strip() for p in rest.split(",")]
        action_name = action_parts[0]
        args = []
        kwargs = {}

        for part in action_parts[1:]:
            if not part:
                continue
            try:
                # 尝试解析为 Python 字面量
                value = ast.literal_eval(part)
                args.append(value)
            except (ValueError, SyntaxError):
                # 作为字符串处理
                args.append(part)

        return MockCommand(
            time_offset=time_offset,
            action_name=action_name,
            args=args,
            kwargs=kwargs,
            line_number=line_num,
        )

    def start(self, on_finished: Optional[Callable] = None):
        """
        开始执行脚本

        Args:
            on_finished: 执行完成回调
        """
        if not self._commands:
            get_logger().warning("No commands to execute")
            return

        self._start_time = time.time()
        self._current_index = 0
        self._is_running = True
        self._results = []
        self._on_finished = on_finished

        get_logger().info(f"[Automation] Started, {len(self._commands)} commands scheduled")

        # 启动定时器 (每 10ms 检查一次)
        self._timer.start(10)

    def stop(self):
        """停止执行"""
        self._timer.stop()
        self._is_running = False
        get_logger().info("[Automation] Stopped")

    def is_running(self) -> bool:
        """是否正在运行"""
        return self._is_running

    def get_results(self) -> list[dict]:
        """获取执行结果"""
        return self._results.copy()

    def _on_tick(self):
        """定时器回调"""
        if self._current_index >= len(self._commands):
            self._finish()
            return

        elapsed = time.time() - self._start_time

        # 执行所有到期的命令
        while self._current_index < len(self._commands):
            cmd = self._commands[self._current_index]

            if cmd.time_offset > elapsed:
                break

            self._execute_command(cmd)
            self._current_index += 1

        # 检查是否完成
        if self._current_index >= len(self._commands):
            # 等待最后一条命令执行后结束
            QTimer.singleShot(100, self._finish)

    def _execute_command(self, cmd: MockCommand):
        """执行单条命令"""
        logger = get_logger()
        logger.info(f"[Automation] +{cmd.time_offset:.3f}s: {cmd.action_name}({cmd.args})")

        result = {
            "command": cmd,
            "success": False,
            "error": None,
            "timestamp": time.time() - self._start_time,
        }

        try:
            # WAIT 命令特殊处理 (在 AutomationController 层面)
            if cmd.action_name == "WAIT":
                # WAIT 在脚本中表示等待，但在时间轴调度中已经处理
                # 这里只是记录
                result["success"] = True
                self._results.append(result)
                return

            # 通过 ActionDispatcher 执行
            self._dispatcher.dispatch(cmd.action_name, *cmd.args, **cmd.kwargs)
            result["success"] = True

        except Exception as e:
            result["error"] = str(e)
            logger.error(f"[Automation] Command failed at line {cmd.line_number}: {e}")

        self._results.append(result)

    def _finish(self):
        """执行完成"""
        self._timer.stop()
        self._is_running = False

        success_count = sum(1 for r in self._results if r["success"])
        fail_count = len(self._results) - success_count

        get_logger().info(
            f"[Automation] Finished: {success_count} success, {fail_count} failed"
        )

        if self._on_finished:
            self._on_finished(self._results)
