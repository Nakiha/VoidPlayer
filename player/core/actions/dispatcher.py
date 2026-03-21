"""
ActionDispatcher - 动作分发器
"""
from typing import Any, Optional, TYPE_CHECKING

from .types import ActionDef, ParamDef, MISSING

if TYPE_CHECKING:
    from player.ui.main_window import MainWindow


class ActionDispatcher:
    """动作分发器 - 统一的命令执行入口"""

    def __init__(self, main_window: "MainWindow"):
        self._mw = main_window
        self._registry: dict[str, ActionDef] = {}

    def register(self, action: ActionDef):
        """注册动作"""
        self._registry[action.name] = action

    def register_batch(self, actions: list[ActionDef]):
        """批量注册动作"""
        for action in actions:
            self.register(action)

    def dispatch(self, action_name: str, *args, **kwargs) -> Any:
        """
        分发并执行动作

        Args:
            action_name: 动作名称
            *args: 位置参数
            **kwargs: 关键字参数

        Returns:
            动作执行结果

        Raises:
            ValueError: 未知动作或缺少必要参数
        """
        action = self._registry.get(action_name)
        if not action:
            raise ValueError(f"Unknown action: {action_name}")

        # 合并参数
        params = self._merge_params(action, args, kwargs)

        # 缺少必要参数时尝试 resolver
        missing = self._get_missing_params(action, params)
        if missing and action.resolver:
            resolved = action.resolver(self._mw)
            if resolved is None:  # 用户取消
                return None
            params.update(resolved)

        # 仍然缺少必要参数 -> 错误
        missing = self._get_missing_params(action, params)
        if missing:
            missing_names = [p.name for p in missing]
            raise ValueError(f"Missing required params: {missing_names}")

        # 校验参数
        self._validate_params(action, params)

        # 执行
        return action.fn(**params)

    def can_dispatch(self, action_name: str) -> bool:
        """检查动作是否可执行 (是否已注册)"""
        return action_name in self._registry

    def get_action_names(self) -> list[str]:
        """获取所有动作名称"""
        return list(self._registry.keys())

    def get_action(self, action_name: str) -> Optional[ActionDef]:
        """获取动作定义"""
        return self._registry.get(action_name)

    def get_action_info(self, action_name: str) -> dict:
        """获取动作详细信息 (用于帮助文档)"""
        action = self._registry.get(action_name)
        if not action:
            return {}

        return {
            "name": action.name,
            "description": action.description,
            "params": [
                {
                    "name": p.name,
                    "type": p.type.__name__,
                    "required": not p.has_default(),
                    "default": p.default if p.has_default() else None,
                }
                for p in action.params
            ],
            "has_resolver": action.resolver is not None,
        }

    def _merge_params(self, action: ActionDef, args: tuple, kwargs: dict) -> dict:
        """合并位置参数和关键字参数"""
        params = {}

        # 处理位置参数
        for i, arg in enumerate(args):
            if i < len(action.params):
                params[action.params[i].name] = arg

        # 处理关键字参数
        params.update(kwargs)

        # 添加默认值
        for p in action.params:
            if p.name not in params and p.has_default():
                params[p.name] = p.default

        return params

    def _get_missing_params(self, action: ActionDef, params: dict) -> list[ParamDef]:
        """获取缺失的必要参数"""
        missing = []
        for p in action.params:
            if p.name not in params and not p.has_default():
                missing.append(p)
        return missing

    def _validate_params(self, action: ActionDef, params: dict):
        """校验参数"""
        param_defs = {p.name: p for p in action.params}

        for name, value in params.items():
            if name not in param_defs:
                continue

            pdef = param_defs[name]

            # 类型检查
            if not isinstance(value, pdef.type):
                # 允许 int 转 float
                if pdef.type == float and isinstance(value, int):
                    continue
                raise TypeError(
                    f"Parameter '{name}' expected {pdef.type.__name__}, got {type(value).__name__}"
                )

            # 自定义校验
            if pdef.validator and not pdef.validate(value):
                raise ValueError(f"Parameter '{name}' validation failed: {value}")
