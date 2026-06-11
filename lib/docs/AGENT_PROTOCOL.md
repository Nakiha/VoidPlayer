# Agent Protocol

> 常驻 agent 控制通道。让 agent 在会话进行中读取人的裁决、查询会话状态、做基础播放控制,
> 是 HITL 评审闭环里"裁决回流给 agent"的通路。传输层沿用 analysis IPC 的模式。

## 启用

默认关闭。启动时传入:

```bash
void_player --agent-connection-file=/path/to/connection.json
```

服务器绑定 `127.0.0.1` 的临时端口,生成一次性 token,并把连接信息**原子写入**
(临时文件 + rename)指定路径:

```json
{ "protocolVersion": 1, "port": 51234, "token": "…", "pid": 4242 }
```

agent 启动播放器后轮询此文件,读取端口和 token 连接。窗口关闭时服务器停止并删除该文件。

## 传输与握手

行分隔 JSON(每行一个对象),与 analysis IPC 相同的 `BoundedLineSplitter` 限长保护。
连接后 5 秒内必须完成握手,否则断开:

```json
→ { "type": "hello", "token": "…" }
← { "type": "helloAck", "protocolVersion": 1 }
```

token 错误、JSON 非法、行超长都会直接断开连接。

## 请求/响应

```json
→ { "id": 1, "method": "getMarks", "params": {} }
← { "id": 1, "result": { … } }
← { "id": 1, "error": { "code": "badRequest", "message": "…" } }
```

`id` 由客户端分配,原样返回。错误码:`badRequest` / `unknownMethod` / `internal`,
以及 handler 定义的业务码。

## 方法清单

| Method | 参数 | 返回 |
|--------|------|------|
| `getSession` | — | `media`(每轨 fileId/slotIndex/path/mediaHash/sourceId)+ `playback`(isPlaying/currentPtsUs/durationUs) |
| `addMedia` | `path` | `{trackCount}`，加载一个媒体文件为新轨道（沙盒平台上路径必须对 app 可读） |
| `getMarks` | — | 裁决导出文档(版本、媒体 lineage、全部标注含裁决字段),与 `EXPORT_MARKS` 输出同构 |
| `exportMarks` | `path` | 把导出文档写到文件,返回 `{path}` |
| `play` / `pause` | — | `{}` |
| `seekTo` | `ptsUs` | `{}` |
| `setMediaSourceId` | `slotIndex`, `sourceId` | `{}`,声明源 lineage,写入 storage catalog |

方法语义全部映射到主窗口 coordinator,见
[main_window_agent.dart](../main_window/main_window_agent.dart);新增方法时在那里扩展,
并更新本表。

## 裁决导出文档

`getMarks` / `exportMarks` / `EXPORT_MARKS` 共用一个版本化 JSON 文档
(`quickMarkExportVersion`,当前 1):

```json
{
  "version": 1,
  "generatedAtMs": 1760000000000,
  "media": [
    { "fileId": 1, "slotIndex": 0, "path": "…", "mediaHash": "…", "sourceId": "clip01" }
  ],
  "marks": [
    {
      "id": 1, "fileId": 1, "mediaHash": "…", "sourceId": "clip01",
      "anchor": { "ptsUs": 2000000, "dtsUs": 1990000, "durationUs": 40000,
                  "analysisFrameIndex": 50, "sourcePacketIndex": -1, "sourcePacketPos": -1 },
      "region": { "left": 0.1, "top": 0.2, "width": 0.3, "height": 0.4 },
      "shape": "rectangle", "text": "sky gradient steps",
      "origin": "human", "defectType": "banding", "severity": 4
    }
  ]
}
```

`region` 是归一化源坐标。裁决字段语义见
[STORAGE_CATALOG.md](STORAGE_CATALOG.md) 的 payload v2 说明。

## 典型 agent 循环

1. agent 产出编码候选,启动播放器:`--agent-connection-file=…`,连上后逐个
   `addMedia` 加载候选、`setMediaSourceId` 声明同源分组。
2. 人评审,在侧边栏标缺陷类型/严重度。
3. agent 轮询 `getMarks`(或会话结束前调 `exportMarks`)收集裁决,按 `sourceId` join
   回编码参数,进入下一轮调参。

## 客户端与 CLI

Python 客户端在 [scripts/dev/agent_client.py](../../scripts/dev/agent_client.py)
(`VoidPlayerAgentClient`),agent 脚本可直接 import;命令行入口:

```bash
# 对运行中的实例执行单个动词
python dev.py agent session --connection-file /path/to/connection.json
python dev.py agent add-media --connection-file … --path /path/to/clip.mp4
python dev.py agent set-source-id --connection-file … --slot 0 --source-id clip01
python dev.py agent marks --connection-file …
python dev.py agent export --connection-file … --path /path/to/verdicts.json

# 真实 app 端到端协议冒烟(macOS)
python dev.py agent-smoke
```

## 安全边界

- 只绑 loopback,不监听外部接口。
- token 每次启动随机生成,只存在于连接文件;文件权限由调用方负责放在受保护目录。
- 方法面有意保持白名单式收窄:agent 不能通过此通道执行任意 Action;扩展能力时逐个
  方法显式加入 `MainWindowAgentHandler`。
