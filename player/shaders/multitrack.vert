#version 330 core
// MultiTrack vertex shader - fullscreen quad

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_texCoord;

out vec2 v_texCoord;
out vec2 v_position;  // 传递屏幕位置用于 aspect ratio 计算

void main() {
    gl_Position = vec4(a_position, 0.0, 1.0);
    v_texCoord = a_texCoord;
    v_position = a_position;
}
