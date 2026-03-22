"""
MultiTrackGLWidget - 多轨道 OpenGL 渲染控件
支持分屏模式 (SPLIT) 和并排模式 (SIDE_BY_SIDE)，切换时不销毁 GL 实例
"""
import ctypes
from enum import IntEnum
from pathlib import Path
from typing import TYPE_CHECKING

from PySide6.QtOpenGLWidgets import QOpenGLWidget
from PySide6.QtGui import QSurfaceFormat, QMouseEvent
from PySide6.QtCore import Signal, Qt, QPointF, QSizeF
from PySide6.QtWidgets import QApplication
from OpenGL.GL import *
from loguru import logger

if TYPE_CHECKING:
    from player.native import voidview_native


class ViewMode(IntEnum):
    """视图模式枚举"""
    SIDE_BY_SIDE = 0  # 并排模式 - 所有视频等分显示
    SPLIT = 1         # 分屏模式 - 只显示前两个，可拖动分割线


class MultiTrackGLWidget(QOpenGLWidget):
    """
    多轨道 OpenGL 渲染控件

    特性:
    - 单一 GL 实例，切换模式不销毁
    - 支持 SPLIT (分屏) 和 SIDE_BY_SIDE (并排) 模式
    - SPLIT 模式支持可拖动分割线
    - 动态 track 顺序
    - 支持 viewport 缩放和移动事件
    """

    MAX_TRACKS = 8

    # 信号
    split_position_changed = Signal(float)  # 分割线位置变化 (0.0 ~ 1.0)
    gl_initialized = Signal()  # OpenGL 上下文初始化完成，可以初始化解码器

    # Viewport 缩放/移动信号
    viewport_wheel_zoom = Signal(int, float, float)  # (delta, mouse_x, mouse_y)
    viewport_pan_start = Signal(float, float)  # (x, y)
    viewport_pan_move = Signal(float, float)   # (x, y)
    viewport_pan_end = Signal()
    viewport_resized = Signal(float, float)    # (width, height)

    def __init__(self, parent=None):
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
        self._gl_initialized = False

        # 分割线拖动状态
        self._dragging_split = False
        self.setMouseTracking(True)

        # Viewport 移动状态
        self._dragging_pan = False
        self._pan_button = Qt.MouseButton.MiddleButton  # 中键拖动移动画面

        # Viewport 缩放/偏移状态 (由 ViewportManager 控制)
        self._zoom_ratio: float = 1.0
        self._view_offset: QPointF = QPointF(0, 0)
        self._track_sizes: list[tuple[int, int]] = [(0, 0)] * self.MAX_TRACKS  # 每个 track 的分辨率

        # 设置 OpenGL 3.3 Core Profile
        fmt = QSurfaceFormat()
        fmt.setVersion(3, 3)
        fmt.setProfile(QSurfaceFormat.OpenGLContextProfile.CoreProfile)
        QSurfaceFormat.setDefaultFormat(fmt)

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
        """设置视图模式 (不销毁 GL 实例)"""
        if self._view_mode == mode:
            return
        self._view_mode = mode
        self.update()

    def set_split_position(self, pos: float):
        """设置分割线位置 (0.0 - 1.0)"""
        self._split_position = max(0.05, min(0.95, pos))
        self.update()

    def set_track_order(self, order: list[int]):
        """设置 track 显示顺序"""
        for i, idx in enumerate(order[:self.MAX_TRACKS]):
            self._track_order[i] = idx
        self.update()

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
        self.update()

    def set_track_count(self, count: int):
        """设置活动 track 数量"""
        self._track_count = min(count, self.MAX_TRACKS)
        self.update()

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
        self.update()

    def set_track_sizes(self, sizes: list[tuple[int, int]]):
        """设置每个 track 的分辨率尺寸

        Args:
            sizes: [(width, height), ...] 列表，索引对应 track 索引
        """
        for i, (w, h) in enumerate(sizes[:self.MAX_TRACKS]):
            self._track_sizes[i] = (w, h)
        self.update()

    # ========== OpenGL 实现 ==========

    def initializeGL(self):
        """初始化 OpenGL 资源"""
        glClearColor(0.0, 0.0, 0.0, 1.0)

        try:
            self._shader_program = self._create_shader_program()
        except Exception as e:
            logger.error(f"Shader initialization failed: {e}")
            return

        # 创建 VAO 和 VBO (全屏四边形)
        # position(2) + texcoord(2) per vertex
        vertices = (ctypes.c_float * 16)(
            -1.0, -1.0,  0.0, 0.0,  # 左下
             1.0, -1.0,  1.0, 0.0,  # 右下
             1.0,  1.0,  1.0, 1.0,  # 右上
            -1.0,  1.0,  0.0, 1.0,  # 左上
        )

        self._vao = glGenVertexArrays(1)
        glBindVertexArray(self._vao)

        self._vbo = glGenBuffers(1)
        glBindBuffer(GL_ARRAY_BUFFER, self._vbo)
        glBufferData(GL_ARRAY_BUFFER, ctypes.sizeof(vertices), vertices, GL_STATIC_DRAW)

        # position attribute (location = 0)
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, ctypes.c_void_p(0))
        glEnableVertexAttribArray(0)
        # texcoord attribute (location = 1)
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, ctypes.c_void_p(8))
        glEnableVertexAttribArray(1)

        glBindVertexArray(0)

        # 创建占位黑色纹理
        for i in range(self.MAX_TRACKS):
            tex = glGenTextures(1)
            glBindTexture(GL_TEXTURE_2D, tex)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes([0, 0, 0, 255]))
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR)
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR)
            self._dummy_textures[i] = tex

        self._gl_initialized = True
        logger.info("MultiTrackGLWidget initialized (GLSL 3.30)")

        # 通知可以初始化解码器了
        self.gl_initialized.emit()

    def paintGL(self):
        """绘制帧"""
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

        # 设置 viewport 缩放和偏移
        glUniform1f(glGetUniformLocation(self._shader_program, "u_zoom_ratio"), self._zoom_ratio)
        glUniform2f(
            glGetUniformLocation(self._shader_program, "u_view_offset"),
            self._view_offset.x(), self._view_offset.y()
        )

        # 绑定纹理
        for i in range(self.MAX_TRACKS):
            glActiveTexture(GL_TEXTURE0 + i)
            decoder = self._decoders[i]
            if decoder and hasattr(decoder, 'texture_id') and decoder.texture_id > 0:
                glBindTexture(GL_TEXTURE_2D, decoder.texture_id)
            else:
                glBindTexture(GL_TEXTURE_2D, self._dummy_textures[i])
            glUniform1i(glGetUniformLocation(self._shader_program, f"u_textures[{i}]"), i)

        # 绘制全屏四边形
        glBindVertexArray(self._vao)
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4)
        glBindVertexArray(0)

        glUseProgram(0)

    def resizeGL(self, w, h):
        """调整视口大小"""
        glViewport(0, 0, w, h)
        # 发出 resize 信号
        self.viewport_resized.emit(float(w), float(h))

    # ========== 分割线拖动 ==========

    def mousePressEvent(self, event):
        """鼠标按下 - 开始拖动分割线或画面移动"""
        # 优先处理分割线拖动
        if self._view_mode == ViewMode.SPLIT and event.button() == Qt.MouseButton.LeftButton:
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
        elif self._view_mode == ViewMode.SPLIT:
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
        split_x = self._split_position * self.width()
        return abs(x - split_x) < 10  # 10px 容差
