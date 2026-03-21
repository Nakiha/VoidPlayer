"""
ActionTypes - 动作系统的数据结构定义
"""
from dataclasses import dataclass, field
from typing import Any, Callable


class _MissingSentinel:
    """表示缺失的参数值"""
    def __repr__(self):
        return "MISSING"


MISSING = _MissingSentinel()


# 动作分类常量
CATEGORY_BASIC = "基础动作"
CATEGORY_PLAYBACK = "播放控制"
CATEGORY_SPEED_ZOOM = "速度/缩放"
CATEGORY_TRACK = "轨道管理"
CATEGORY_VIEW = "视图控制"
CATEGORY_DEBUG = "调试/诊断"
CATEGORY_ASSERT = "断言动作 (测试用)"


@dataclass
class ParamDef:
    """参数定义"""
    name: str                    # 参数名
    type: type                   # 参数类型 (int, str, float, ...)
    default: Any = MISSING       # 默认值 (MISSING 表示必须提供)
    validator: Callable[[Any], bool] = None   # 校验函数

    def has_default(self) -> bool:
        """是否有默认值"""
        return self.default is not MISSING

    def validate(self, value: Any) -> bool:
        """校验参数值"""
        if self.validator is None:
            return True
        return self.validator(value)

    def format_type(self) -> str:
        """格式化参数显示 (用于帮助文档)"""
        type_name = self.type.__name__
        if self.has_default():
            return f"{self.name}: {type_name} = {self.default}"
        return f"{self.name}: {type_name}"


@dataclass
class ActionDef:
    """动作定义"""
    name: str                              # 动作名称 (大写下划线)
    fn: Callable                           # 执行函数
    params: list[ParamDef] = field(default_factory=list)  # 参数定义
    description: str = ""                  # 描述 (用于帮助文档)
    category: str = CATEGORY_BASIC         # 分类
    resolver: Callable = None              # 参数解析器 (用于交互式获取参数)

    def get_required_params(self) -> list[ParamDef]:
        """获取必需参数列表"""
        return [p for p in self.params if not p.has_default()]

    def get_param_names(self) -> list[str]:
        """获取所有参数名"""
        return [p.name for p in self.params]

    def format_params(self) -> str:
        """格式化参数显示 (用于帮助文档)"""
        if not self.params:
            return "-"
        return ", ".join(p.format_type() for p in self.params)


def get_actions_by_category(actions: list[ActionDef]) -> dict[str, list[ActionDef]]:
    """按分类获取动作列表"""
    result: dict[str, list[ActionDef]] = {}
    for action in actions:
        cat = action.category
        if cat not in result:
            result[cat] = []
        result[cat].append(action)
    return result
