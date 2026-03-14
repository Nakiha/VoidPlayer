"""
时间格式化工具函数
"""


def format_time_seconds(seconds: float) -> str:
    """将秒数格式化为 SS.CC 格式 (秒.百分秒)"""
    abs_sec = abs(seconds)
    sec = int(abs_sec)
    centisec = int((abs_sec - sec) * 100)
    return f"{sec:02d}.{centisec:02d}"


def format_time_ms(ms: int, show_sign: bool = False) -> str:
    """
    将毫秒格式化为时间字符串

    格式规则:
    - 默认显示: 00:00
    - 有毫秒时显示: SS.SSS 或 MM:SS.SSS 或 H:MM:SS.SSS
    - 超过1分钟: MM:SS[.SSS]
    - 超过1小时: H:MM:SS[.SSS] (小时数可以超过24)

    Args:
        ms: 毫秒值
        show_sign: 是否显示正负号 (+/-)
    """
    # 确定符号
    if ms < 0:
        sign = "-"
        abs_ms = abs(ms)
    elif show_sign and ms > 0:
        sign = "+"
        abs_ms = ms
    else:
        sign = ""
        abs_ms = ms

    # 特殊情况：0值
    if abs_ms == 0:
        return f"{sign}00:00"

    # 计算各部分
    total_seconds = abs_ms // 1000
    milliseconds = abs_ms % 1000
    hours = total_seconds // 3600
    minutes = (total_seconds % 3600) // 60
    seconds = total_seconds % 60

    # 判断是否需要显示毫秒（毫秒不为0时显示）
    has_ms = milliseconds != 0
    ms_str = f".{milliseconds:03d}" if has_ms else ""

    if hours > 0:
        # 超过1小时: H:MM:SS[.SSS]
        return f"{sign}{hours}:{minutes:02d}:{seconds:02d}{ms_str}"
    elif minutes > 0:
        # 超过1分钟: MM:SS[.SSS]
        return f"{sign}{minutes:02d}:{seconds:02d}{ms_str}"
    else:
        # 不足1分钟: SS[.SSS]
        return f"{sign}{seconds:02d}{ms_str}"
