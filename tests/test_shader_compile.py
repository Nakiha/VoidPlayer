#!/usr/bin/env python3
"""
Shader compilation test - Debug GLSL shaders independently
"""
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).parent.parent
sys.path.insert(0, str(PROJECT_ROOT / "player"))

from PySide6.QtWidgets import QApplication
from PySide6.QtOpenGLWidgets import QOpenGLWidget
from PySide6.QtGui import QSurfaceFormat
from OpenGL.GL import *


class ShaderTestWidget(QOpenGLWidget):
    """Minimal widget to test shader compilation"""

    def __init__(self):
        super().__init__()
        self.shader_program = None
        self.vao = None
        self.vbo = None

    def initializeGL(self):
        """Test shader compilation"""
        print("=" * 60)
        print("Testing shader compilation (GLSL 3.30)...")
        print("=" * 60)

        gl_version = glGetString(GL_VERSION)
        glsl_version = glGetString(GL_SHADING_LANGUAGE_VERSION)
        print(f"OpenGL Version: {gl_version.decode() if gl_version else 'N/A'}")
        print(f"GLSL Version: {glsl_version.decode() if glsl_version else 'N/A'}")
        print()

        shader_dir = PROJECT_ROOT / "player" / "shaders"
        vert_path = shader_dir / "multitrack.vert"
        frag_path = shader_dir / "multitrack.frag"

        try:
            # Read shader source
            with open(vert_path, 'r', encoding='utf-8') as f:
                vert_source = f.read()
            with open(frag_path, 'r', encoding='utf-8') as f:
                frag_source = f.read()

            print("--- Vertex Shader ---")
            for i, line in enumerate(vert_source.split('\n'), 1):
                print(f"{i:3}: {line}")
            print()

            print("--- Fragment Shader ---")
            for i, line in enumerate(frag_source.split('\n'), 1):
                print(f"{i:3}: {line}")
            print()

            # Compile vertex shader
            print("Compiling vertex shader...")
            vertex_shader = glCreateShader(GL_VERTEX_SHADER)
            glShaderSource(vertex_shader, vert_source)
            glCompileShader(vertex_shader)

            if not glGetShaderiv(vertex_shader, GL_COMPILE_STATUS):
                error = glGetShaderInfoLog(vertex_shader).decode()
                print(f"  FAILED: {error}")
                return
            print("  SUCCESS")

            # Compile fragment shader
            print("Compiling fragment shader...")
            fragment_shader = glCreateShader(GL_FRAGMENT_SHADER)
            glShaderSource(fragment_shader, frag_source)
            glCompileShader(fragment_shader)

            if not glGetShaderiv(fragment_shader, GL_COMPILE_STATUS):
                error = glGetShaderInfoLog(fragment_shader).decode()
                print(f"  FAILED: {error}")
                return
            print("  SUCCESS")

            # Link program
            print("Linking shader program...")
            self.shader_program = glCreateProgram()
            glAttachShader(self.shader_program, vertex_shader)
            glAttachShader(self.shader_program, fragment_shader)
            glLinkProgram(self.shader_program)

            if not glGetProgramiv(self.shader_program, GL_LINK_STATUS):
                error = glGetProgramInfoLog(self.shader_program).decode()
                print(f"  FAILED: {error}")
                return
            print("  SUCCESS")

            # Cleanup shaders
            glDeleteShader(vertex_shader)
            glDeleteShader(fragment_shader)

            # Create VAO and VBO for fullscreen quad
            # position(2) + texcoord(2) per vertex
            vertices = [
                # pos        texcoord
                -1.0, -1.0,  0.0, 0.0,
                 1.0, -1.0,  1.0, 0.0,
                 1.0,  1.0,  1.0, 1.0,
                -1.0,  1.0,  0.0, 1.0,
            ]

            self.vao = glGenVertexArrays(1)
            glBindVertexArray(self.vao)

            self.vbo = glGenBuffers(1)
            glBindBuffer(GL_ARRAY_BUFFER, self.vbo)
            glBufferData(GL_ARRAY_BUFFER, len(vertices) * 4,  # float32
                        (ctypes.c_float * len(vertices))(*vertices), GL_STATIC_DRAW)

            # position attribute
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, ctypes.c_void_p(0))
            glEnableVertexAttribArray(0)
            # texcoord attribute
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, ctypes.c_void_p(8))
            glEnableVertexAttribArray(1)

            glBindVertexArray(0)

            print()
            print("=" * 60)
            print("ALL TESTS PASSED!")
            print("=" * 60)

        except Exception as e:
            print(f"ERROR: {e}")
            import traceback
            traceback.print_exc()

    def paintGL(self):
        glClearColor(0.1, 0.1, 0.1, 1.0)
        glClear(GL_COLOR_BUFFER_BIT)

        if self.shader_program and self.vao:
            glUseProgram(self.shader_program)

            # Set uniforms
            glUniform1i(glGetUniformLocation(self.shader_program, "u_mode"), 0)
            glUniform1i(glGetUniformLocation(self.shader_program, "u_track_count"), 2)
            glUniform1f(glGetUniformLocation(self.shader_program, "u_split_pos"), 0.5)
            for i in range(8):
                glUniform1i(glGetUniformLocation(self.shader_program, f"u_order[{i}]"), i)

            # Create dummy textures
            for i in range(8):
                tex = glGenTextures(1)
                glActiveTexture(GL_TEXTURE0 + i)
                glBindTexture(GL_TEXTURE_2D, tex)
                # Different colors for each texture
                color = [int(i * 30), int(100 + i * 20), int(50 + i * 10), 255]
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes(color))
                glUniform1i(glGetUniformLocation(self.shader_program, f"u_textures[{i}]"), i)

            # Draw
            glBindVertexArray(self.vao)
            glDrawArrays(GL_TRIANGLE_FAN, 0, 4)
            glBindVertexArray(0)
            glUseProgram(0)


def main():
    import ctypes

    fmt = QSurfaceFormat()
    fmt.setVersion(3, 3)
    fmt.setProfile(QSurfaceFormat.OpenGLContextProfile.CoreProfile)
    QSurfaceFormat.setDefaultFormat(fmt)

    app = QApplication(sys.argv)

    widget = ShaderTestWidget()
    widget.resize(400, 300)
    widget.show()

    # Auto-close after 2 seconds if successful
    def check_close():
        if widget.shader_program:
            print("\nWindow will close in 2 seconds...")
            from PySide6.QtCore import QTimer
            QTimer.singleShot(2000, app.quit)

    from PySide6.QtCore import QTimer
    QTimer.singleShot(500, check_close)

    return app.exec()


if __name__ == "__main__":
    import ctypes
    sys.exit(main())
