# App Feedback

Flutter 侧的轻量运行时通知统一走 `AppFeedbackController` /
`AppFeedbackHost`。

适合使用 `AppFeedback` 的场景：

- 用户操作已被接受、跳过或失败，但不需要阻断当前工作流。
- 清理缓存、打开网络/远程媒体、性能压力提示等短生命周期反馈。
- 可以在几秒后自动消失，或只需要一个轻量 action 的提示。

不适合使用 `AppFeedback` 的场景：

- 需要用户输入或确认的流程，继续使用 dialog。
- 播放后端不可用、首个媒体加载失败等会阻断 viewport 的状态，继续使用
  `ViewportDisplayState.error`。
- 控件自身的 hover、tooltip、selection、validation 状态，这些应留在对应
  widget 或 view model 内。

不要直接新增 `ScaffoldMessenger`、一次性 toast 或临时 overlay 来做全局提示。
需要新提示时，把事件从业务层冒泡到拥有 `BuildContext` 的 UI 层，再通过
`AppFeedbackScope.read(context)` 显示。
