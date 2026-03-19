"""
Diagnostics - 性能诊断模块

提供:
- PerformanceMonitor: 解码性能统计
- DiagnosticsManager: 诊断管理器 (导出、UI协调)
- StatsOverlay: 性能统计 UI 面板
"""
from player.core.diagnostics.performance_monitor import PerformanceMonitor
from player.core.diagnostics.diagnostics_manager import DiagnosticsManager

__all__ = ["PerformanceMonitor", "DiagnosticsManager"]
