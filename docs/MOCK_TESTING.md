# Mock 测试规范

## 文件格式 (.vpmock)

Mock 测试文件用于自动化 UI 交互测试，格式为纯文本：

```
# 注释行 (以 # 开头)
时间偏移(秒), 动作名称[, 参数1, 参数2, ...]
```

### 示例

```csv
# 基础播放测试
0.5, PLAY
2.0, PAUSE
2.5, SEEK_TO, 1000
10.0, QUIT, 0
```

### 规则

1. **时间偏移**: 从程序启动开始的秒数 (浮点数)
2. **动作名称**: 大写下划线格式，见 `--list-actions`
3. **参数**: 逗号分隔，类型自动推断 (int/str/float)
4. **空行和注释**: 被忽略

## 运行方式

```bash
# 运行 mock 测试
python run_player.py --mock tests/mock/basic_playback.vpmock

# 列出所有可用动作
python run_player.py --list-actions
```

## 测试文件组织

```
tests/mock/
├── basic_playback.vpmock   # 播放/暂停/seek
├── speed_zoom.vpmock       # 速度和缩放
├── frame_navigation.vpmock # 帧导航
└── assertions.vpmock       # 断言测试
```

## 新增交互功能的测试清单

完成一个新功能时，确保：

1. **动作注册**: 在 `player/core/action_registry.py` 的两处添加：
   - `get_action_metadata()` - 元数据 (用于 --list-actions)
   - `create_action_registry()` - 执行逻辑

2. **快捷键绑定** (如有): 在 `player/core/shortcuts.py` 添加：
   - `ShortcutAction` 枚举
   - `ACTION_NAME_MAP` 映射
   - `DEFAULT_BINDINGS` 定义

3. **Mock 测试**: 在 `tests/mock/` 创建或更新测试文件
   - 测试正常流程
   - 测试边界情况

## 断言动作

用于验证程序状态，失败时抛出异常：

| 动作 | 参数 | 说明 |
|------|------|------|
| `ASSERT_PLAYING` | - | 断言正在播放 |
| `ASSERT_PAUSED` | - | 断言已暂停 |
| `ASSERT_POSITION` | expected_ms, tolerance_ms=100 | 断言播放位置 |
| `ASSERT_TRACK_COUNT` | expected | 断言轨道数量 |

示例：
```csv
1.0, PLAY
1.5, ASSERT_PLAYING
3.0, SEEK_TO, 5000
3.5, ASSERT_POSITION, 5000, 100
```

## 参数类型

| 类型 | 示例 | 说明 |
|------|------|------|
| int | `5000` | 整数，常用于时间(毫秒) |
| str | `resources/video/file.mp4` | 字符串，**必须使用正斜杠** |
| float | `1.5` | 浮点数 |
| list | 不支持 | 复杂类型需在代码中处理 |

**注意**: Windows 路径必须使用正斜杠 `/`，因为反斜杠会被解析为转义字符 (如 `\v` → 垂直制表符)。
