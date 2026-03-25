"""
MultiTrackGLWindow - 多轨道 OpenGL 渲染窗口
使用 QOpenGLWindow 替代 QOpenGLWidget 以获得更好的性能
使用 SSBO (Shader Storage Buffer Object) 替代传统 glUniform 调用
"""
import ctypes
import hashlib
import os
from enum import IntEnum
from pathlib import Path
from typing import TYPE_CHECKING

import numpy as np
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


# SSBO 数据结构 - 匹配 std430 布局
# 注意：numpy dtype 需要手动对齐以匹配 GLSL std430 规则
# - 标量 (int32, float32): 4 字节对齐
# - vec2: 8 字节对齐，由两个 float32 组成
# - 数组: 元素紧密排列
# 总大小: 168 bytes
SSBO_VIEW_DATA_DTYPE = np.dtype([
    # === 固定参数 (offset 0-15) ===
    ('u_mode', np.int32),           # offset 0
    ('u_track_count', np.int32),    # offset 4
    ('u_split_pos', np.float32),    # offset 8
    ('u_zoom_ratio', np.float32),   # offset 12

    # === 画布参数 (offset 16-31) ===
    ('u_canvas_aspect', np.float32),  # offset 16
    ('_pad_canvas_size', np.float32), # offset 20: padding for vec2 8-byte alignment
    ('u_canvas_size_x', np.float32),  # offset 24: vec2[0]
    ('u_canvas_size_y', np.float32),  # offset 28: vec2[1]

    # === 视图偏移 (offset 32-39) ===
    ('u_view_offset_x', np.float32),  # offset 32: vec2[0]
    ('u_view_offset_y', np.float32),  # offset 36: vec2[1]

    # === Track 顺序数组 (offset 40-71) ===
    ('u_order', np.int32, (8,)),      # offset 40

    # === Track 宽高比数组 (offset 72-103) ===
    ('u_aspect_ratios', np.float32, (8,)),  # offset 72

    # === Track 分辨率数组 (offset 104-167) ===
    # vec2[8] 在 std430 中是紧密排列的，每个 vec2 8 字节
    ('u_track_sizes', np.float32, (8, 2)),  # offset 104
])


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
        self._ssbo = None  # Shader Storage Buffer Object
        self._ssbo_ptr = None  # 持久化映射的指针
        self._view_data = None  # numpy array view of mapped SSBO memory
        self._sampler_locs = None  # 缓存的 sampler uniform locations
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

    def _get_app_dir(self) -> Path:
        """获取应用程序目录（兼容 Nuitka 打包）"""
        import sys
        # Nuitka 打包后 __compiled__ 存在
        if "__compiled__" in globals() or hasattr(sys, 'frozen'):
            # 打包后：使用可执行文件所在目录
            return Path(sys.executable).parent
        else:
            # 开发模式：使用项目根目录
            return Path(__file__).parent.parent.parent.parent

    def _load_shader_source(self, filename: str) -> str:
        """从文件加载 shader 源码（兼容 Nuitka 打包）"""
        shader_path = self._get_app_dir() / "player" / "shaders" / filename
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

    def _get_shader_cache_path(self) -> Path:
        """获取 shader 缓存目录路径（兼容 Nuitka 打包）"""
        import sys

        # 打包后使用用户数据目录
        if "__compiled__" in globals() or hasattr(sys, 'frozen'):
            # Windows: %LOCALAPPDATA%/VoidPlayer/cache/shaders
            app_data = Path(os.environ.get('LOCALAPPDATA', Path.home() / 'AppData' / 'Local'))
            cache_dir = app_data / "VoidPlayer" / "cache" / "shaders"
        else:
            # 开发模式：使用项目根目录
            cache_dir = Path(__file__).parent.parent.parent.parent / "cache" / "shaders"

        cache_dir.mkdir(parents=True, exist_ok=True)
        return cache_dir

    def _compute_shader_hash(self, vert_source: str, frag_source: str) -> str:
        """计算 shader 源码的 hash 作为缓存 key"""
        # 包含 OpenGL 版本和渲染器信息，驱动更新后缓存失效
        gl_version = glGetString(GL_VERSION).decode() if glGetString(GL_VERSION) else "unknown"
        gl_renderer = glGetString(GL_RENDERER).decode() if glGetString(GL_RENDERER) else "unknown"
        combined = f"{vert_source}\n{frag_source}\n{gl_version}\n{gl_renderer}"
        return hashlib.sha256(combined.encode()).hexdigest()[:16]

    def _try_load_cached_program(self, cache_file: Path) -> int | None:
        """尝试从缓存加载预编译的 shader program"""
        if not cache_file.exists():
            return None

        try:
            with open(cache_file, "rb") as f:
                data = f.read()

            # 解析 header: 4 bytes binary format + 4 bytes binary length
            if len(data) < 8:
                return None

            binary_format = int.from_bytes(data[:4], "little")
            binary_length = int.from_bytes(data[4:8], "little")
            binary_data = data[8:]

            if len(binary_data) != binary_length:
                logger.debug(f"Cache file size mismatch: expected {binary_length}, got {len(binary_data)}")
                return None

            # 创建 program 并加载 binary
            program = glCreateProgram()
            glProgramBinary(program, binary_format, binary_data, len(binary_data))

            # 检查是否成功
            if glGetProgramiv(program, GL_LINK_STATUS):
                logger.info(f"Loaded shader from cache: {cache_file.name}")
                return program
            else:
                glDeleteProgram(program)
                return None

        except Exception as e:
            logger.debug(f"Failed to load cached shader: {e}")
            return None

    def _save_program_cache(self, program: int, cache_file: Path):
        """保存编译后的 shader program 到缓存"""
        try:
            # 检查 program 是否有效
            if not glIsProgram(program):
                logger.debug("Invalid program, skip caching")
                return

            # 确保 program 已链接 - 使用 ctypes 直接调用
            link_status = ctypes.c_int()
            glGetProgramiv(program, GL_LINK_STATUS, ctypes.byref(link_status))
            if not link_status.value:
                logger.debug("Program not linked, skip caching")
                return

            # 获取 binary 长度 - 使用 ctypes 直接调用
            binary_length = ctypes.c_int()
            glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, ctypes.byref(binary_length))
            length_val = binary_length.value
            logger.debug(f"Program binary length: {length_val}")

            if not length_val or length_val <= 0:
                logger.debug(f"Program binary length is {length_val}, skip caching")
                return

            # 清除之前的 OpenGL 错误
            while glGetError() != GL_NO_ERROR:
                pass

            # 准备 buffer
            binary_data = ctypes.create_string_buffer(length_val)
            binary_format = ctypes.c_uint()
            actual_length = ctypes.c_size_t()

            # 调用 glGetProgramBinary
            glGetProgramBinary(
                program,
                length_val,
                ctypes.byref(actual_length),
                ctypes.byref(binary_format),
                binary_data
            )

            # 检查 OpenGL 错误
            err = glGetError()
            if err != GL_NO_ERROR:
                err_names = {1280: "INVALID_ENUM", 1281: "INVALID_VALUE", 1282: "INVALID_OPERATION"}
                logger.warning(f"glGetProgramBinary failed: GL_{err_names.get(err, f'ERROR_{err}')}")
                return

            if actual_length.value == 0:
                logger.debug("glGetProgramBinary returned 0 bytes")
                return

            # 写入文件: format(4) + length(4) + data
            with open(cache_file, "wb") as f:
                f.write(binary_format.value.to_bytes(4, "little"))
                f.write(actual_length.value.to_bytes(4, "little"))
                f.write(binary_data.raw[:actual_length.value])

            logger.info(f"Saved shader cache: {cache_file.name} ({actual_length.value} bytes)")

        except Exception as e:
            logger.warning(f"Failed to save shader cache: {e}")

    def _create_shader_program(self) -> int:
        """创建并链接 shader 程序 (支持预编译缓存)"""
        vert_source = self._load_shader_source("multitrack.vert")
        frag_source = self._load_shader_source("multitrack.frag")

        # 计算 hash 并检查缓存
        shader_hash = self._compute_shader_hash(vert_source, frag_source)
        cache_file = self._get_shader_cache_path() / f"multitrack_{shader_hash}.bin"

        # 尝试加载缓存
        program = self._try_load_cached_program(cache_file)
        if program:
            return program

        logger.info("Compiling shaders (cache miss or invalid)")
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

        # 保存到缓存
        self._save_program_cache(program, cache_file)

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

    def _gl_debug_callback(self, source, msg_type, msg_id, severity, length, message, user_param):
        """OpenGL Debug Output 回调函数"""
        # 映射 severity 到日志级别
        severity_map = {
            GL_DEBUG_SEVERITY_HIGH: logger.error,
            GL_DEBUG_SEVERITY_MEDIUM: logger.warning,
            GL_DEBUG_SEVERITY_LOW: logger.info,
            GL_DEBUG_SEVERITY_NOTIFICATION: logger.debug,
        }

        # 映射 type 到可读名称
        type_names = {
            GL_DEBUG_TYPE_ERROR: "ERROR",
            GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: "DEPRECATED",
            GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR: "UNDEFINED",
            GL_DEBUG_TYPE_PORTABILITY: "PORTABILITY",
            GL_DEBUG_TYPE_PERFORMANCE: "PERFORMANCE",
            GL_DEBUG_TYPE_MARKER: "MARKER",
            GL_DEBUG_TYPE_PUSH_GROUP: "PUSH_GROUP",
            GL_DEBUG_TYPE_POP_GROUP: "POP_GROUP",
            GL_DEBUG_TYPE_OTHER: "OTHER",
        }

        # 映射 source 到可读名称
        source_names = {
            GL_DEBUG_SOURCE_API: "API",
            GL_DEBUG_SOURCE_WINDOW_SYSTEM: "WINDOW_SYSTEM",
            GL_DEBUG_SOURCE_SHADER_COMPILER: "SHADER_COMPILER",
            GL_DEBUG_SOURCE_THIRD_PARTY: "THIRD_PARTY",
            GL_DEBUG_SOURCE_APPLICATION: "APPLICATION",
            GL_DEBUG_SOURCE_OTHER: "OTHER",
        }

        log_func = severity_map.get(severity, logger.debug)
        type_name = type_names.get(msg_type, f"UNKNOWN({msg_type})")
        source_name = source_names.get(source, f"UNKNOWN({source})")

        msg_str = ctypes.cast(message, ctypes.c_char_p).value.decode() if message else ""

        # 过滤掉一些通知级别的消息
        if severity == GL_DEBUG_SEVERITY_NOTIFICATION and "Buffer detailed info" in msg_str:
            return

        log_func(f"[GL] {source_name}:{type_name}(0x{msg_id:X}): {msg_str}")

    def _setup_debug_output(self):
        """设置 OpenGL Debug Output (仅 opengl=DEBUG/TRACE 时启用)"""
        from player.core.config import config

        # 检查是否启用了 OpenGL debug 日志级别
        if not config.is_opengl_debug_enabled:
            return

        # 检查是否支持 GL_KHR_debug 或 OpenGL 4.3+
        version = glGetString(GL_VERSION)
        if not version:
            return

        try:
            # 启用 debug output
            glEnable(GL_DEBUG_OUTPUT)
            glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS)  # 同步模式，在调用点立即触发回调

            # 使用 PyOpenGL 提供的 GLDEBUGPROC 类型
            self._debug_callback = GLDEBUGPROC(self._gl_debug_callback)
            glDebugMessageCallback(self._debug_callback, None)

            # 配置消息控制：过滤掉通知级别的冗余消息
            # 只启用 ERROR, DEPRECATED, UNDEFINED, PORTABILITY, PERFORMANCE
            glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_ERROR, GL_DONT_CARE, 0, None, GL_TRUE)
            glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR, GL_DONT_CARE, 0, None, GL_TRUE)
            glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR, GL_DONT_CARE, 0, None, GL_TRUE)
            glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_PORTABILITY, GL_DONT_CARE, 0, None, GL_TRUE)
            glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_PERFORMANCE, GL_DONT_CARE, 0, None, GL_TRUE)
            # 禁用通知类型（通常是冗余的提示）
            glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_OTHER, GL_DONT_CARE, 0, None, GL_FALSE)
            glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_MARKER, GL_DONT_CARE, 0, None, GL_FALSE)

            logger.debug("OpenGL Debug Output enabled (opengl=DEBUG)")

        except Exception as e:
            logger.debug(f"OpenGL Debug Output not available: {e}")

    def initializeGL(self):
        """初始化 OpenGL 资源 (QOpenGLWindow 方法名)"""
        glClearColor(0.0, 0.0, 0.0, 1.0)

        # 设置 Debug Output (开发阶段启用)
        self._setup_debug_output()

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

        # 创建 SSBO (Shader Storage Buffer Object) - DSA + 持久化映射
        ssbo = ctypes.c_uint()
        glCreateBuffers(1, ctypes.byref(ssbo))
        self._ssbo = ssbo.value

        # 计算 buffer 大小
        buffer_size = SSBO_VIEW_DATA_DTYPE.itemsize

        # 使用 glNamedBufferStorage 创建不可变存储 + 持久化映射标志
        # GL_MAP_WRITE_BIT: 允许写入
        # GL_MAP_PERSISTENT_BIT: 映射在 buffer 生命周期内保持有效
        # GL_MAP_COHERENT_BIT: 写入对 GPU 立即可见，无需手动 flush
        flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT
        glNamedBufferStorage(self._ssbo, buffer_size, None, flags)

        # 持久化映射 - 一次性映射，永久使用
        self._ssbo_ptr = glMapNamedBufferRange(self._ssbo, 0, buffer_size, flags)

        if self._ssbo_ptr is None:
            raise RuntimeError("Failed to map SSBO buffer")

        # 创建 numpy 数组视图，直接映射到 GPU 内存
        # 这样 _view_data 的修改会直接反映到 GPU
        self._view_data = np.frombuffer(
            (ctypes.c_byte * buffer_size).from_address(self._ssbo_ptr),
            dtype=SSBO_VIEW_DATA_DTYPE
        )

        # 绑定 SSBO 到 binding point 0 (与 shader 中的 binding = 0 对应)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, self._ssbo)

        # 缓存 sampler uniform locations (sampler 不能放在 SSBO 中)
        # 一次性获取，避免每帧调用 glGetUniformLocation
        self._sampler_locs = [
            glGetUniformLocation(self._shader_program, f"u_textures[{i}]")
            for i in range(self.MAX_TRACKS)
        ]

        self._gl_initialized = True
        logger.info(f"MultiTrackGLWindow initialized (GLSL 4.50, DSA, persistent-mapped SSBO {buffer_size} bytes)")

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

        # 更新 SSBO 数据 (一次性更新所有参数)
        view_data = self._view_data[0]
        view_data['u_mode'] = int(self._view_mode)
        view_data['u_track_count'] = self._track_count
        view_data['u_split_pos'] = self._split_position
        view_data['u_zoom_ratio'] = self._zoom_ratio
        view_data['u_canvas_aspect'] = self.width() / max(self.height(), 1)

        # u_canvas_size (vec2)
        view_data['u_canvas_size_x'] = float(self.width())
        view_data['u_canvas_size_y'] = float(self.height())

        # u_view_offset (vec2)
        view_data['u_view_offset_x'] = self._view_offset.x()
        view_data['u_view_offset_y'] = self._view_offset.y()

        # u_order 数组
        for i in range(self.MAX_TRACKS):
            view_data['u_order'][i] = self._track_order[i]

        # u_aspect_ratios 数组
        for i in range(self.MAX_TRACKS):
            view_data['u_aspect_ratios'][i] = self._aspect_ratios[i]

        # u_track_sizes 数组 (8 x 2)
        for i in range(self.MAX_TRACKS):
            w, h = self._track_sizes[i]
            view_data['u_track_sizes'][i, 0] = float(w)
            view_data['u_track_sizes'][i, 1] = float(h)

        # 注意：由于使用持久化映射 + GL_MAP_COHERENT_BIT，
        # _view_data 的修改直接反映到 GPU 内存，无需调用 glNamedBufferSubData

        # 设置 sampler uniform (sampler 不能放在 SSBO 中，使用缓存的 location)
        for i in range(self.MAX_TRACKS):
            glUniform1i(self._sampler_locs[i], i)

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
