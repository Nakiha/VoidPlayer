"""
MultiTrackGLWindow - 多轨道 OpenGL 渲染窗口
使用 QOpenGLWindow 替代 QOpenGLWidget 以获得更好的性能
"""
import ctypes
from enum import IntEnum
from pathlib import Path
from typing import TYPE_CHECKING

from PySide6.QtGui import QSurfaceFormat
from PySide6.QtCore import Signal, Qt, QPointF, QObject
from PySide6.QtOpenGL import QOpenGLWindow
from PySide6.QtWidgets import QApplication
from OpenGL.GL import *
from loguru import logger

# DSA 函数在 OpenGL.GL 中，需要确保加载
# PyOpenGL 会自动加载 OpenGL 4.5 函数

if TYPE_CHECKING:
    from player.native import voidview_native


class ViewMode(IntEnum):
    """视图模式枚举"""
    SIDE_BY_SIDE = 0  # 并排模式 - 所有视频等分显示
    SPLIT_SCREEN = 1  # 分屏对比模式 - 重叠显示，分割线切换


class MultiTrackGLWindow(QOpenGLWindow):
    """
    多轨道 OpenGL 渲染窗口

    特性:
    - 使用 QOpenGLWindow 避免 QOpenGLWidget 的 FBO 开销
    - 支持 SPLIT (分屏) 和 SIDE_BY_SIDE (并排) 模式
    - SPLIT 模式支持可拖动分割线
    - 动态 track 顺序
    - 支持 viewport 缩放和移动事件

    注意: QOpenGLWindow 不是 QWidget，需要通过 createWindowContainer() 嵌入
    """

    MAX_TRACKS = 8

    # 信号 (通过 QObject 信号机制)
    split_position_changed = Signal(float)  # 分割线位置变化 (0.0 ~ 1.0)
    gl_initialized = Signal()  # OpenGL 上下文初始化完成，可以初始化解码器

    # Viewport 缩放/移动信号
    viewport_wheel_zoom = Signal(int, float, float)  # (delta, mouse_x, mouse_y)
    viewport_pan_start = Signal(float, float)  # (x, y)
    viewport_pan_move = Signal(float, float)   # (x, y)
    viewport_pan_end = Signal()
    viewport_resized = Signal(float, float)    # (width, height)

    def __init__(self, parent=None):
        # QOpenGLWindow 的 parent 是 QWindow，不是 QWidget
        super().__init__(parent)

        self._view_mode = ViewMode.SIDE_BY_SIDE
        self._split_position = 0.5
        self._track_order = list(range(self.MAX_TRACKS))
        self._track_count = 0
        self._decoders: list["voidview_native.HardwareDecoder | None"] = [None] * self.MAX_TRACKS
        self._aspect_ratios = [0.0] * self.MAX_TRACKS  # 每个视频的宽高比

        # OpenGL 资源
        self._shader_program = None
        self._vao = None
        self._vbo = None
        self._dummy_textures = [0] * self.MAX_TRACKS
        self._texture_ids = None  # 用于 multi-bind 的 ctypes 数组
        self._gl_initialized = False

        # 分割线拖动状态
        self._dragging_split = False

        # Viewport 移动状态
        self._dragging_pan = False
        self._pan_button = Qt.MouseButton.MiddleButton  # 中键拖动移动画面

        # Viewport 缩放/偏移状态 (由 ViewportManager 控制)
        self._zoom_ratio: float = 1.0
        self._view_offset: QPointF = QPointF(0, 0)
        self._track_sizes: list[tuple[int, int]] = [(0, 0)] * self.MAX_TRACKS  # 每个 track 的分辨率

        # 鼠标位置缓存 (用于事件处理)
        self._last_mouse_pos = QPointF()

        # 关闭标志（程序退出时设置为 True，阻止后续渲染）
        self._is_closing = False

    def cleanup(self):
        """清理资源，在窗口关闭前调用"""
        self._is_closing = True

    def _load_shader_source(self, filename: str) -> str:
        """从文件加载 shader 源码"""
        shader_path = Path(__file__).parent.parent.parent / "shaders" / filename
        with open(shader_path, 'r', encoding='utf-8') as f:
            return f.read()

    def _compile_shader(self, source: str, shader_type: int) -> int:
        """编译单个 shader"""
        shader = glCreateShader(shader_type)
        glShaderSource(shader, source)
        glCompileShader(shader)

        if not glGetShaderiv(shader, GL_COMPILE_STATUS):
            error = glGetShaderInfoLog(shader).decode()
            glDeleteShader(shader)
            raise RuntimeError(f"Shader compile error: {error}")

        return shader

    def _create_shader_program(self) -> int:
        """创建并链接 shader 程序"""
        vert_source = self._load_shader_source("multitrack.vert")
        frag_source = self._load_shader_source("multitrack.frag")

        vertex_shader = self._compile_shader(vert_source, GL_VERTEX_SHADER)
        fragment_shader = self._compile_shader(frag_source, GL_FRAGMENT_SHADER)

        program = glCreateProgram()
        glAttachShader(program, vertex_shader)
        glAttachShader(program, fragment_shader)
        glLinkProgram(program)

        if not glGetProgramiv(program, GL_LINK_STATUS):
            error = glGetProgramInfoLog(program).decode()
            glDeleteProgram(program)
            glDeleteShader(vertex_shader)
            glDeleteShader(fragment_shader)
            raise RuntimeError(f"Shader link error: {error}")

        glDeleteShader(vertex_shader)
        glDeleteShader(fragment_shader)

        return program

    # ========== 公共 API ==========

    def set_view_mode(self, mode: ViewMode):
        """设置视图模式"""
        if self._view_mode == mode:
            return
        self._view_mode = mode
        self.requestUpdate()

    def set_split_position(self, pos: float):
        """设置分割线位置 (0.0 - 1.0)"""
        self._split_position = max(0.05, min(0.95, pos))
        self.requestUpdate()

    def set_track_order(self, order: list[int]):
        """设置 track 显示顺序"""
        for i, idx in enumerate(order[:self.MAX_TRACKS]):
            self._track_order[i] = idx
        self.requestUpdate()

    def set_decoders(self, decoders: list["voidview_native.HardwareDecoder | None"]):
        """设置解码器列表

        注意: 此方法不修改 _track_count，track 数量由 set_track_count 控制
        """
        self._decoders = [None] * self.MAX_TRACKS
        self._aspect_ratios = [0.0] * self.MAX_TRACKS
        for i, dec in enumerate(decoders[:self.MAX_TRACKS]):
            self._decoders[i] = dec
            # 从解码器获取宽高比
            if dec:
                try:
                    w, h = dec.width, dec.height
                    if w > 0 and h > 0:
                        self._aspect_ratios[i] = w / h
                except Exception as e:
                    logger.error(f"set_decoders[{i}]: Failed to get width/height: {e}")
        self.requestUpdate()

    def set_track_count(self, count: int):
        """设置活动 track 数量"""
        self._track_count = min(count, self.MAX_TRACKS)
        self.requestUpdate()

    @property
    def view_mode(self) -> ViewMode:
        return self._view_mode

    @property
    def is_gl_initialized(self) -> bool:
        """OpenGL 上下文是否已初始化"""
        return self._gl_initialized

    @property
    def split_position(self) -> float:
        return self._split_position

    # ========== Viewport 缩放/偏移 API ==========

    def set_viewport_transform(self, zoom_ratio: float, view_offset: QPointF):
        """设置 viewport 缩放和偏移"""
        self._zoom_ratio = zoom_ratio
        self._view_offset = view_offset
        self.requestUpdate()

    def set_track_sizes(self, sizes: list[tuple[int, int]]):
        """设置每个 track 的分辨率尺寸

        Args:
            sizes: [(width, height), ...] 列表，索引对应 track 索引
        """
        for i, (w, h) in enumerate(sizes[:self.MAX_TRACKS]):
            self._track_sizes[i] = (w, h)
        self.requestUpdate()

    # ========== OpenGL 实现 ==========

    def initializeGL(self):
        """初始化 OpenGL 资源 (QOpenGLWindow 方法名)"""
        glClearColor(0.0, 0.0, 0.0, 1.0)

        try:
            self._shader_program = self._create_shader_program()
        except Exception as e:
            logger.error(f"Shader initialization failed: {e}")
            return

        # 创建 VAO 和 VBO (全屏四边形) - DSA 方式
        # position(2) + texcoord(2) per vertex
        vertices = (ctypes.c_float * 16)(
            -1.0, -1.0,  0.0, 0.0,  # 左下
             1.0, -1.0,  1.0, 0.0,  # 右下
             1.0,  1.0,  1.0, 1.0,  # 右上
            -1.0,  1.0,  0.0, 1.0,  # 左上
        )

        # 创建 VAO (DSA)
        vao = ctypes.c_uint()
        glCreateVertexArrays(1, ctypes.byref(vao))
        self._vao = vao.value

        # 创建 VBO (DSA)
        vbo = ctypes.c_uint()
        glCreateBuffers(1, ctypes.byref(vbo))
        self._vbo = vbo.value
        glNamedBufferData(self._vbo, ctypes.sizeof(vertices), vertices, GL_STATIC_DRAW)

        # 设置顶点属性 (DSA)
        # 绑定 VBO 到 VAO 的 binding point 0
        glVertexArrayVertexBuffer(self._vao, 0, self._vbo, 0, 16)

        # position attribute (location = 0)
        glEnableVertexArrayAttrib(self._vao, 0)
        glVertexArrayAttribFormat(self._vao, 0, 2, GL_FLOAT, GL_FALSE, 0)
        glVertexArrayAttribBinding(self._vao, 0, 0)

        # texcoord attribute (location = 1)
        glEnableVertexArrayAttrib(self._vao, 1)
        glVertexArrayAttribFormat(self._vao, 1, 2, GL_FLOAT, GL_FALSE, 8)
        glVertexArrayAttribBinding(self._vao, 1, 0)

        # 创建占位黑色纹理 (DSA 方式)
        for i in range(self.MAX_TRACKS):
            tex = ctypes.c_uint()
            glCreateTextures(GL_TEXTURE_2D, 1, ctypes.byref(tex))
            self._dummy_textures[i] = tex.value
            glTextureStorage2D(self._dummy_textures[i], 1, GL_RGBA8, 1, 1)
            glTextureSubImage2D(self._dummy_textures[i], 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bytes([0, 0, 0, 255]))
            glTextureParameteri(self._dummy_textures[i], GL_TEXTURE_MIN_FILTER, GL_LINEAR)
            glTextureParameteri(self._dummy_textures[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR)

        # 预分配 texture IDs 数组用于 multi-bind
        self._texture_ids = (ctypes.c_uint * self.MAX_TRACKS)()

        self._gl_initialized = True
        logger.info("MultiTrackGLWindow initialized (GLSL 4.50, DSA)")

        # 通知可以初始化解码器了
        self.gl_initialized.emit()

    def paintGL(self):
        """绘制帧 (QOpenGLWindow 方法名)"""
        from player.core.logging_config import get_logger
        pts_list = []
        for i, d in enumerate(self._decoders):
            if d and d.texture_id > 0:
                pts_list.append(f"track{i}:{d.current_pts_ms}ms")
        get_logger().debug(f"[GL] paintGL called, textures={[d.texture_id if d else 0 for d in self._decoders]}, pts={pts_list}")
        if self._is_closing:
            return
        glClear(GL_COLOR_BUFFER_BIT)

        if not self._shader_program or not self._vao:
            return

        glUseProgram(self._shader_program)

        # 设置 uniform 变量
        glUniform1i(glGetUniformLocation(self._shader_program, "u_mode"), int(self._view_mode))
        glUniform1i(glGetUniformLocation(self._shader_program, "u_track_count"), self._track_count)
        glUniform1f(glGetUniformLocation(self._shader_program, "u_split_pos"), self._split_position)

        # 设置 track 顺序
        for i in range(self.MAX_TRACKS):
            loc = glGetUniformLocation(self._shader_program, f"u_order[{i}]")
            glUniform1iv(loc, 1, (ctypes.c_int * 1)(self._track_order[i]))

        # 设置宽高比
        for i in range(self.MAX_TRACKS):
            loc = glGetUniformLocation(self._shader_program, f"u_aspect_ratios[{i}]")
            glUniform1f(loc, self._aspect_ratios[i])

        # 设置每个 track 的分辨率尺寸
        for i in range(self.MAX_TRACKS):
            w, h = self._track_sizes[i]
            loc = glGetUniformLocation(self._shader_program, f"u_track_sizes[{i}]")
            glUniform2f(loc, float(w), float(h))

        # 设置画布宽高比
        canvas_aspect = self.width() / max(self.height(), 1)
        glUniform1f(glGetUniformLocation(self._shader_program, "u_canvas_aspect"), canvas_aspect)

        # 设置画布尺寸 (像素)，用于分割线渲染
        glUniform2f(
            glGetUniformLocation(self._shader_program, "u_canvas_size"),
            float(self.width()), float(self.height())
        )

        # 设置 viewport 缩放和偏移
        glUniform1f(glGetUniformLocation(self._shader_program, "u_zoom_ratio"), self._zoom_ratio)
        glUniform2f(
            glGetUniformLocation(self._shader_program, "u_view_offset"),
            self._view_offset.x(), self._view_offset.y()
        )

        # 绑定纹理 (multi-bind)
        for i in range(self.MAX_TRACKS):
            decoder = self._decoders[i]
            if decoder and hasattr(decoder, 'texture_id') and decoder.texture_id > 0:
                self._texture_ids[i] = decoder.texture_id
            else:
                self._texture_ids[i] = self._dummy_textures[i]
        glBindTextures(0, self.MAX_TRACKS, self._texture_ids)

        # 绘制全屏四边形
        glBindVertexArray(self._vao)
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4)
        glBindVertexArray(0)

        glUseProgram(0)

        # 强制刷新渲染流水线
        glFinish()

    def resizeGL(self, w, h):
        """调整视口大小 (QOpenGLWindow 方法名)"""
        if self._is_closing:
            return
        glViewport(0, 0, w, h)
        self.viewport_resized.emit(float(w), float(h))

    # ========== 鼠标事件 ==========

    def mousePressEvent(self, event):
        """鼠标按下 - 开始拖动分割线或画面移动"""
        self._last_mouse_pos = event.position()

        # 优先处理分割线拖动
        if self._view_mode == ViewMode.SPLIT_SCREEN and event.button() == Qt.MouseButton.LeftButton:
            if self._is_near_split_line(event.position().x()):
                self._dragging_split = True
                QApplication.setOverrideCursor(Qt.CursorShape.SizeHorCursor)
                return

        # 处理画面移动（中键）
        if event.button() == self._pan_button:
            self._dragging_pan = True
            pos = event.position()
            self.viewport_pan_start.emit(pos.x(), pos.y())
            QApplication.setOverrideCursor(Qt.CursorShape.ClosedHandCursor)
            return

        super().mousePressEvent(event)

    def mouseMoveEvent(self, event):
        """鼠标移动 - 拖动分割线、画面移动或更新光标"""
        self._last_mouse_pos = event.position()

        # 处理画面移动
        if self._dragging_pan:
            pos = event.position()
            self.viewport_pan_move.emit(pos.x(), pos.y())
            return

        # 处理分割线拖动
        if self._dragging_split:
            new_pos = event.position().x() / self.width()
            self.set_split_position(new_pos)
            self.split_position_changed.emit(self._split_position)
        elif self._view_mode == ViewMode.SPLIT_SCREEN:
            if self._is_near_split_line(event.position().x()):
                QApplication.setOverrideCursor(Qt.CursorShape.SizeHorCursor)
            else:
                QApplication.restoreOverrideCursor()
        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event):
        """鼠标释放 - 结束拖动"""
        # 结束画面移动
        if event.button() == self._pan_button and self._dragging_pan:
            self._dragging_pan = False
            self.viewport_pan_end.emit()
            QApplication.restoreOverrideCursor()
            return

        # 结束分割线拖动
        if self._dragging_split:
            self._dragging_split = False
            QApplication.restoreOverrideCursor()
            return
        super().mouseReleaseEvent(event)

    def wheelEvent(self, event):
        """滚轮事件 - 缩放"""
        # 检查是否有修饰键（如果有则不处理，让父组件处理）
        modifiers = event.modifiers()
        if modifiers != Qt.KeyboardModifier.NoModifier:
            super().wheelEvent(event)
            return

        # 发出缩放信号
        delta = event.angleDelta().y()  # 正数=上滚，负数=下滚
        pos = event.position()
        self.viewport_wheel_zoom.emit(delta, pos.x(), pos.y())
        event.accept()

    def leaveEvent(self, event):
        """鼠标离开 - 恢复光标"""
        if self._dragging_split:
            self._dragging_split = False
        if self._dragging_pan:
            self._dragging_pan = False
            self.viewport_pan_end.emit()
        QApplication.restoreOverrideCursor()
        super().leaveEvent(event)

    def _is_near_split_line(self, x: float) -> bool:
        """判断是否在分割线附近"""
        if self.width() == 0:
            return False
        if self._view_mode != ViewMode.SPLIT_SCREEN:
            return False
        split_x = self._split_position * self.width()
        return abs(x - split_x) < 10  # 10px 容差


# 保持向后兼容的别名
MultiTrackGLWidget = MultiTrackGLWindow
