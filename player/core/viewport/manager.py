"""
ViewportManager - Viewport 状态管理

管理缩放比例和画面偏移，支持多轨道同步缩放/移动
"""
from dataclasses import dataclass
from typing import Optional, TYPE_CHECKING

from loguru import logger
from PySide6.QtCore import QObject, Signal, QPointF, QSizeF, QRectF

from .track_layout import (
    TrackLayout, TrackRegion, LayoutMode,
    calculate_fit_ratio, normalize_mouse_position
)

if TYPE_CHECKING:
    from player.ui.widgets.zoom_combo_box import ZoomComboBox


@dataclass
class TrackInfo:
    """轨道视频信息"""
    index: int           # track 索引
    width: int           # 片分辨率宽度
    height: int          # 片分辨率高度

    @property
    def size(self) -> QSizeF:
        return QSizeF(self.width, self.height)

    @property
    def pixel_count(self) -> int:
        return self.width * self.height


class ViewportManager(QObject):
    """Viewport 状态管理

    管理缩放比例和画面偏移，支持多轨道同步缩放/移动

    核心概念:
    - 缩放比例 (zoom_ratio): 1.0 = 原始大小，2.0 = 放大 2 倍
    - 视图偏移 (view_offset): 片坐标系中的左上角位置
    - fit 值: 锁定画面宽高比填充分割视图最短边所需的缩放比例
    - 像素当量: 视频一个像素在分割视图中占据的实际像素数（所有 track 统一）

    状态变化触发点:
    - 滚轮缩放 → 更新 zoom_ratio，可能更新 view_offset
    - ComboBox 缩放 → 更新 zoom_ratio，可能更新 view_offset
    - 鼠标拖动 → 更新 view_offset
    - 窗体 resize → 重新计算 fit，可能钳制 zoom/offset
    - 布局模式变化 → 更新分割视图，可能钳制
    - 分割比调整 → 更新分割视图，可能钳制
    - Add/Remove track → 检查 max 变化，可能转换坐标系
    """

    # 信号
    zoom_changed = Signal(float)        # 缩放比例变化
    offset_changed = Signal(QPointF)    # 偏移变化
    viewport_changed = Signal()         # 整体视口变化（需要重绘）

    # 缩放步进因子
    ZOOM_STEP_FACTOR = 0.94  # 滚轮每步缩放因子

    def __init__(self, parent=None):
        super().__init__(parent)

        # 核心状态
        self._zoom_ratio: float = 1.0        # 当前缩放比例
        self._view_offset: QPointF = QPointF(0, 0)  # 视图偏移（片坐标系）

        # 轨道信息
        self._tracks: list[TrackInfo] = []   # track 信息列表
        self._max_track: Optional[TrackInfo] = None  # 当前最大分辨率 track（缓存）

        # 布局
        self._track_layout = TrackLayout()
        self._widget_size: QSizeF = QSizeF(0, 0)

        # UI 控件引用（可选）
        self._zoom_combo: Optional["ZoomComboBox"] = None

    # --- 属性 ---

    @property
    def zoom_ratio(self) -> float:
        return self._zoom_ratio

    @property
    def view_offset(self) -> QPointF:
        return self._view_offset

    @property
    def track_layout(self) -> TrackLayout:
        return self._track_layout

    @property
    def max_track(self) -> Optional[TrackInfo]:
        return self._max_track

    # --- 初始化/配置 ---

    def set_zoom_combo(self, combo: "ZoomComboBox"):
        """设置 ZoomComboBox 控件引用"""
        self._zoom_combo = combo

    def set_layout_mode(self, mode: LayoutMode):
        """设置布局模式"""
        if self._track_layout.mode != mode:
            self._track_layout.set_mode(mode)
            self._on_layout_changed()

    def set_split_ratio(self, ratio: float):
        """设置分屏模式的分割比例 (0.1 ~ 0.9)"""
        old_ratio = self._track_layout.split_ratio
        self._track_layout.set_split_ratio(ratio)
        if old_ratio != self._track_layout.split_ratio:
            self._on_layout_changed()

    # --- Track 管理 ---

    def set_tracks(self, tracks: list[tuple[int, int, int]]):
        """设置 track 列表

        Args:
            tracks: [(index, width, height), ...] 列表
        """
        self._tracks.clear()
        for index, width, height in tracks:
            if width > 0 and height > 0:
                self._tracks.append(TrackInfo(index, width, height))

        self._update_max_track()
        self._on_layout_changed()

    def add_track(self, index: int, width: int, height: int):
        """添加 track"""
        if width <= 0 or height <= 0:
            return

        new_track = TrackInfo(index, width, height)
        old_max = self._max_track
        self._tracks.append(new_track)

        # 更新布局
        self._track_layout.update_layout(self._widget_size, len(self._tracks))

        # 检查最大分辨率 track 是否变化
        new_max = self._get_max_track()
        if new_max == new_track and new_track != old_max:
            self._on_max_track_changed(old_max, new_max)

        self._max_track = new_max
        self._update_zoom_combo()
        self.viewport_changed.emit()

    def remove_track(self, index: int):
        """移除 track"""
        removed_track = None
        for i, track in enumerate(self._tracks):
            if track.index == index:
                removed_track = self._tracks.pop(i)
                break

        if removed_track is None:
            return

        old_max = self._max_track

        # 更新布局
        self._track_layout.update_layout(self._widget_size, len(self._tracks))

        if len(self._tracks) == 0:
            # 无 track，重置状态
            self._zoom_ratio = 1.0
            self._view_offset = QPointF(0, 0)
            self._max_track = None
        else:
            new_max = self._get_max_track()
            if removed_track == old_max:
                # 最大分辨率 track 被移除
                self._on_max_track_changed(old_max, new_max)
            self._max_track = new_max

        self.viewport_changed.emit()

    def _update_max_track(self):
        """更新最大分辨率 track 缓存"""
        self._max_track = self._get_max_track()

    def _get_max_track(self) -> Optional[TrackInfo]:
        """获取最大分辨率的 track"""
        if not self._tracks:
            return None
        return max(self._tracks, key=lambda t: t.pixel_count)

    # --- Widget 尺寸变化 ---

    def on_widget_resize(self, new_size: QSizeF):
        """处理控件大小变化"""
        if self._widget_size == new_size:
            return

        self._widget_size = new_size

        # 更新布局
        self._track_layout.update_layout(new_size, len(self._tracks))

        if not self._tracks:
            return

        # 更新 zoom combo 的 fit 值
        self._update_zoom_combo()

        # 触发重绘
        self.viewport_changed.emit()

    def _on_layout_changed(self):
        """布局变化处理"""
        self._track_layout.update_layout(self._widget_size, len(self._tracks))

        if self._tracks:
            # 更新 zoom combo 的 fit 值
            self._update_zoom_combo()

        self.viewport_changed.emit()

    # --- 缩放操作 ---

    def get_min_zoom(self) -> float:
        """获取最小缩放比例（最大分辨率 track 的 fit 值）"""
        if not self._max_track:
            return 0.1  # 默认最小值

        viewport = self._track_layout.get_viewport_region(self._max_track.index)
        if viewport is None:
            return 0.1

        return calculate_fit_ratio(self._max_track.size, viewport.size())

    def apply_wheel_zoom(self, delta: int, mouse_widget_pos: QPointF):
        """应用滚轮缩放

        Args:
            delta: 滚轮增量（正=上滚放大，负=下滚缩小）
            mouse_widget_pos: QOpenGLWidget 坐标系中的鼠标位置
        """
        if not self._max_track:
            return

        # 计算新缩放比例
        if delta > 0:
            new_zoom = self._zoom_ratio / self.ZOOM_STEP_FACTOR  # 放大
        else:
            new_zoom = self._zoom_ratio * self.ZOOM_STEP_FACTOR  # 缩小

        # 执行缩放
        self._apply_zoom_internal(new_zoom, mouse_widget_pos)

    def apply_zoom_ratio(self, zoom_ratio: float):
        """应用指定缩放比例（从 ComboBox 触发）

        Args:
            zoom_ratio: 目标缩放比例
        """
        if not self._max_track:
            return

        # 无鼠标位置，使用中心缩放
        self._apply_zoom_internal(zoom_ratio, None)

    def reset_zoom(self):
        """重置缩放到 fit"""
        if not self._max_track:
            return

        fit_zoom = self.get_min_zoom()
        self._apply_zoom_internal(fit_zoom, None)

    def _apply_zoom_internal(
        self,
        zoom_ratio: float,
        mouse_widget_pos: Optional[QPointF],
    ):
        """内部缩放应用"""
        if not self._max_track:
            return

        old_zoom = self._zoom_ratio
        self._zoom_ratio = zoom_ratio

        # 计算缩放因子（用于偏移计算）
        if old_zoom > 0:
            zoom_factor = old_zoom / zoom_ratio
        else:
            zoom_factor = 1.0

        # 获取鼠标所在 track
        if mouse_widget_pos:
            track_index = self._track_layout.get_track_at(mouse_widget_pos)
        else:
            track_index = -1

        # 获取最大 track 的视口区域
        max_viewport = self._track_layout.get_viewport_region(self._max_track.index)
        if max_viewport is None:
            self._update_zoom_combo()
            self.zoom_changed.emit(zoom_ratio)
            self.viewport_changed.emit()
            return

        # 检查是否有黑边
        has_black_bars = self._check_black_bars(self._max_track, max_viewport, zoom_ratio)

        if has_black_bars or mouse_widget_pos is None:
            # 有黑边或无鼠标位置时以中心缩放，重置偏移
            self._view_offset = QPointF(0, 0)
        else:
            # 无黑边时以鼠标位置缩放
            mouse_norm = normalize_mouse_position(mouse_widget_pos, max_viewport)
            logger.debug(f"[ZOOM] mouse_widget=({mouse_widget_pos.x():.1f}, {mouse_widget_pos.y():.1f}), "
                        f"max_viewport=({max_viewport.x():.1f}, {max_viewport.y():.1f}, {max_viewport.width():.1f}x{max_viewport.height():.1f}), "
                        f"mouse_norm=({mouse_norm.x():.3f}, {mouse_norm.y():.3f})")
            self._view_offset = self._calculate_zoom_offset(
                zoom_factor, mouse_norm, self._max_track, max_viewport
            )

        self._update_zoom_combo()
        self.zoom_changed.emit(zoom_ratio)
        self.viewport_changed.emit()

    def _check_black_bars(
        self,
        track: TrackInfo,
        viewport: QRectF,
        zoom_ratio: float,
    ) -> bool:
        """检查是否有黑边"""
        display_w = track.width * zoom_ratio
        display_h = track.height * zoom_ratio
        return display_w < viewport.width() or display_h < viewport.height()

    def _calculate_zoom_offset(
        self,
        zoom_factor: float,
        mouse_norm: QPointF,
        max_track: TrackInfo,
        max_viewport: QRectF,
    ) -> QPointF:
        """计算缩放后的视图偏移

        缩放逻辑：
        - 归一化坐标 (0,0) 表示分割视图左上角，朝左上角缩放
        - 归一化坐标 (1,1) 表示分割视图右下角，朝右下角缩放
        - 朝左上角缩放：取画面 x[0,w), y[0,h) 部分
        - 朝右下角缩放：取画面 x[offset_x,offset_x+w), y[offset_y,offset_y+h) 部分

        注意：shader 中画面是居中显示的，view_offset 是相对于居中位置的偏移

        Args:
            zoom_factor: old_zoom / new_zoom
            mouse_norm: 鼠标在分割视图中的归一化位置 (0~1, 0~1)
            max_track: 最大分辨率 track
            max_viewport: 最大 track 的视口区域
        """
        fit_ratio = calculate_fit_ratio(max_track.size, max_viewport.size())
        video_w, video_h = max_track.width, max_track.height

        # 当前 zoom 下，画面在片坐标系中的可见范围（像素）
        current_view_w = max_viewport.width() / (fit_ratio * self._zoom_ratio)
        current_view_h = max_viewport.height() / (fit_ratio * self._zoom_ratio)

        # 缩放前的可见范围
        old_view_w = current_view_w * zoom_factor
        old_view_h = current_view_h * zoom_factor

        # 可见范围变化量（放大时 dw,dh > 0，表示可见范围变小了）
        dw = old_view_w - current_view_w
        dh = old_view_h - current_view_h

        # 关键：计算当前居中显示时，画面左上角在片坐标系中的位置
        # 居中时：center_offset = (video_size - view_size) / 2
        # 例如：视频 1920x1080，可见范围 1280x720，居中偏移 = (320, 180)
        old_center_x = (video_w - old_view_w) / 2
        old_center_y = (video_h - old_view_h) / 2
        new_center_x = (video_w - current_view_w) / 2
        new_center_y = (video_h - current_view_h) / 2

        # 缩放后，根据鼠标位置调整偏移
        # mouse_norm = 0 → 保持左上角不变 → 偏移应该使得左上角仍可见
        # mouse_norm = 1 → 保持右下角不变 → 偏移应该使得右下角仍可见
        #
        # 新偏移 = 新居中偏移 + (旧偏移 - 旧居中偏移) * 缩放因子 + 调整量
        # 简化：保持鼠标所指的点在屏幕上位置不变
        # 新偏移 = 旧偏移 + dw * (mouse_norm - 0.5)
        #
        # 更直观的理解：
        # mouse_norm = 0 时，我们希望画面的左上角保持可见
        # mouse_norm = 1 时，我们希望画面的右下角保持可见
        # mouse_norm = 0.5 时，保持中心不变

        # 方案：基于"鼠标所指的画面位置在屏幕上不动"来计算
        # 1. 鼠标在屏幕上的归一化位置 = mouse_norm
        # 2. 缩放前，鼠标指向的画面位置（片坐标）= old_center + mouse_norm * old_view
        # 3. 缩放后，要让同一画面位置仍显示在 mouse_norm 处，需要调整偏移

        # 缩放前后居中偏移的变化
        center_dx = new_center_x - old_center_x
        center_dy = new_center_y - old_center_y

        # 根据鼠标位置，需要额外偏移多少来补偿
        # mouse_norm = 0 → 补偿 -center_d（左上角缩放）
        # mouse_norm = 1 → 补偿 +center_d（右下角缩放）
        # mouse_norm = 0.5 → 补偿 0（中心不变）
        adjust_x = center_dx * (2 * mouse_norm.x() - 1)
        adjust_y = center_dy * (2 * mouse_norm.y() - 1)

        new_x = self._view_offset.x() + adjust_x
        new_y = self._view_offset.y() + adjust_y

        logger.debug(f"[OFFSET] old_view=({old_view_w:.1f}, {old_view_h:.1f}), "
                    f"current_view=({current_view_w:.1f}, {current_view_h:.1f}), "
                    f"dw=({dw:.1f}, {dh:.1f}), "
                    f"center_d=({center_dx:.1f}, {center_dy:.1f}), "
                    f"adjust=({adjust_x:.1f}, {adjust_y:.1f}), "
                    f"new_offset=({new_x:.1f}, {new_y:.1f})")

        return QPointF(new_x, new_y)

    # --- 移动操作 ---

    def apply_pan(self, delta: QPointF):
        """应用画面移动

        Args:
            delta: 屏幕像素移动量（鼠标移动方向）
        """
        if not self._max_track:
            return

        max_viewport = self._track_layout.get_viewport_region(self._max_track.index)
        if max_viewport is None:
            return

        # 将屏幕像素移动转换为片坐标移动
        # 转换链：屏幕像素 * (1 / 实际显示比例) = 视频像素
        # 实际显示比例 = fit_ratio * zoom_ratio
        fit_ratio = calculate_fit_ratio(self._max_track.size, max_viewport.size())
        display_ratio = fit_ratio * self._zoom_ratio

        # 鼠标拖 1 像素 = 视频移动 1/display_ratio 像素
        scale = 1.0 / display_ratio
        pan_x = delta.x() * scale
        pan_y = delta.y() * scale

        # 计算新偏移
        # 期望：鼠标往哪拖，画面往哪走
        self._view_offset = QPointF(
            self._view_offset.x() + pan_x,
            self._view_offset.y() + pan_y,
        )

        self.offset_changed.emit(self._view_offset)
        self.viewport_changed.emit()

    # --- 辅助方法 ---

    def _on_max_track_changed(self, old_max: Optional[TrackInfo], new_max: Optional[TrackInfo]):
        """处理最大分辨率 track 变化"""
        if old_max is None:
            # 首次添加 track，自动 fit
            self._zoom_ratio = self.get_min_zoom()
            self._view_offset = QPointF(0, 0)
            self._update_zoom_combo()
            return

        if new_max is None:
            return

        # 转换偏移到新坐标系
        self._view_offset = self._convert_offset(old_max, new_max)

        self._update_zoom_combo()

    def _convert_offset(
        self,
        old_max: TrackInfo,
        new_max: TrackInfo,
    ) -> QPointF:
        """转换偏移到新坐标系"""
        scale_x = new_max.width / old_max.width
        scale_y = new_max.height / old_max.height

        return QPointF(
            self._view_offset.x() * scale_x,
            self._view_offset.y() * scale_y,
        )

    def _update_zoom_combo(self):
        """更新 ZoomComboBox 显示"""
        if self._zoom_combo:
            self._zoom_combo.set_fit_value(self.get_min_zoom())
            self._zoom_combo.set_zoom_ratio(self._zoom_ratio, emit=False)
