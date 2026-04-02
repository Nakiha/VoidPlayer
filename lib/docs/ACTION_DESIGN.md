# Action 系统设计文档

> 统一管理用户操作的抽象层：快捷键、UI 按钮、测试脚本共享同一套 Action 定义。

## 设计目标

1. **单一来源** — 每个 PlayerAction 只定义一次，快捷键和按钮共用
2. **按键拦截** — 仅在 `bind` 到 `unbind` 期间拦截快捷键，阻止 Flutter 框架捕获
3. **全局一致** — 快捷键行为不随焦点位置改变，无论焦点在哪个控件上效果都相同
4. **可脚本化测试** — 基于时间线的 CSV 脚本驱动 Action 和 Assert，无需人工交互

## 类型体系

```dart
/// 用户可执行的操作
sealed class PlayerAction {
  final String name;
  final LogicalKeyboardKey? shortcut; // null = 无快捷键
  const PlayerAction(this.name, [this.shortcut]);
}

// 有快捷键、无参数
class Play extends PlayerAction {
  const Play() : super('PLAY', LogicalKeyboardKey.space);
}

// 无快捷键、有参数
class SeekTo extends PlayerAction {
  final int ptsUs;
  const SeekTo(this.ptsUs) : super('SEEK_TO');
}

// 更多 Action 见 ACTION_MAINTENANCE.md 清单

/// 状态断言（测试用）
sealed class PlayerAssert {
  const PlayerAssert();
}

class AssertPlaying extends PlayerAssert { const AssertPlaying(); }
class AssertPosition extends PlayerAssert {
  final int ptsUs;
  final int toleranceMs;
  const AssertPosition(this.ptsUs, this.toleranceMs);
}
// 更多 Assert 见 ACTION_MAINTENANCE.md 清单
```

完整清单见 [ACTION_MAINTENANCE.md](ACTION_MAINTENANCE.md)。

## 核心组件

### ActionRegistry — 全局注册表

```dart
class ActionRegistry {
  /// name → PlayerAction 定义（bind 时自动索引）
  final Map<String, PlayerAction> _actions = {};

  /// name → callback
  final Map<String, VoidCallback> _callbacks = {};

  /// LogicalKey → action name（反向索引，bind/unbind 时维护）
  final Map<LogicalKeyboardKey, String> _keyMap = {};

  /// 绑定 action + callback，同时索引快捷键
  void bind(PlayerAction action, VoidCallback callback);

  /// 解绑，移除 callback 和快捷键索引
  void unbind(String name);

  /// 主动执行（UI 按钮、测试脚本调用）
  void execute(String name);

  /// ActionFocus 的 KeyEvent handler
  KeyEventResult handleKey(KeyEvent event);
}
```

**无需 `register`。** `bind` 同时完成 action 定义注册和回调挂载：

- `bind(Play(), controller.play)` → 索引 `Space → 'PLAY'`，存储 callback
- `unbind('PLAY')` → 移除 callback，删除 `Space` 的拦截索引

**两阶段生命周期：**

| 阶段 | 快捷键行为 | callback |
|------|-----------|----------|
| `bind` 前 | 放行（Flutter 框架正常处理） | 无 |
| `bind` 后 | **拦截**（吞掉，执行回调） | 有 |
| `unbind` 后 | 恢复放行 | 无 |

**未绑定的 action 调用：** `execute` 发现 callback 为 null 时，打印 `log.severe('Action "$name" not bound')`，静默返回。测试脚本触发未绑定的 action 也能在日志中看到。

### ActionFocus — 全局按键拦截层

```dart
class ActionFocus extends StatelessWidget {
  final Widget child;
  // 内部: Focus(onKeyEvent: registry.handleKey, autofocus: true)
}
```

**拦截逻辑（`handleKey`）：**

```
按键事件
  │
  ├─ 焦点在 EditableText 上？ → ignored（放行给输入框）
  │
  ├─ 按键不在 _keyMap 中？   → ignored
  │
  ├─ 查 _callbacks[name]
  │   ├─ 存在 → 执行回调，返回 handled（吞掉）
  │   └─ null → 返回 ignored（放行）
  │
  ▼ (ignored 时 Flutter 框架默认处理生效)
```

**EditableText 穿透：** 焦点在 `TextField` 等输入控件内时放行所有按键，保证光标移动、文本编辑等基本功能正常。除此之外不做其他焦点判断，快捷键行为一致。

### 使用示例

```dart
// bind（控件渲染时）
@override
void initState() {
  super.initState();
  actionRegistry.bind(const Play(), controller.play);  // Space 开始拦截
}

// unbind（控件销毁时）
@override
void dispose() {
  actionRegistry.unbind('PLAY');  // Space 恢复放行
  super.dispose();
}

// UI 按钮同源
FloatingActionButton(
  onPressed: () => actionRegistry.execute('PLAY'),
  child: const Icon(Icons.play_arrow),
)
```

## 测试脚本格式

```csv
# 注释行
时间(秒), 指令 [, 参数...]
```

指令分三类：

| 类别 | 指令 | 说明 |
|------|------|------|
| Action | `PLAY`, `PAUSE`, `SEEK_TO ptsUs`, `SET_SPEED speed`, ... | 对应 PlayerAction，调用 controller |
| Wait | `WAIT_PLAYING timeoutMs`, `WAIT_PAUSED timeoutMs` | 轮询状态直到满足或超时 |
| Assert | `ASSERT_PLAYING`, `ASSERT_POSITION ptsUs toleranceMs`, ... | 断言当前状态，失败则 throw |
| Control | `QUIT exitCode` | 退出测试 |

完整指令列表见 [ACTION_MAINTENANCE.md](ACTION_MAINTENANCE.md)。

### 示例

```csv
0.0, OPEN_FILE, test_video.mp4
0.5, WAIT_PLAYING, 3000
1.0, ASSERT_PLAYING
2.0, PAUSE
2.5, ASSERT_PAUSED
3.0, SEEK_TO, 5000000
3.5, ASSERT_POSITION, 5000000, 100000
5.0, PLAY
10.0, QUIT, 0
```

### TestRunner

```dart
class TestRunner {
  final String scriptPath;
  final VideoRendererController controller;

  /// 从 CLI 参数 --test-script <path> 触发
  Future<void> run();
}
```

执行：解析脚本 → 按时间调度 → Action 调 controller / Assert 读状态 / Wait 轮询 → QUIT 退出。

## 文件结构（规划）

```
lib/
├── docs/
│   ├── ACTION_DESIGN.md
│   └── ACTION_MAINTENANCE.md
├── actions/
│   ├── player_action.dart
│   ├── player_assert.dart
│   ├── action_registry.dart
│   └── test_runner.dart
├── video_renderer_controller.dart
└── main.dart
```

## 架构位置

```
main.dart
  ├─ MyApp
  │   └─ ActionFocus           ← 全局按键拦截，常驻
  │       └─ VideoPlayerPage
  │           └─ initState → bind / dispose → unbind
  │           └─ Button → execute('PLAY')
  │
  └─ --test-script 模式
      └─ TestRunnerApp
          └─ TestRunner         ← 绕过 UI 直接调 controller
```

Action 层位于 UI 和 Controller 之间。Controller 本身不依赖 Action。
