# Storage Catalog

本文档约定 Flutter 侧运行时存储、标注数据和可重建缓存的落盘格式。标注和
analysis 目前没有发布过对外稳定版本，本轮采用 breaking change：旧的
`annotations/local.vpmarks` 与 `marks/thumbnails/` 不再迁移，也不再读取。

## 目录

运行时根目录由 `AppPaths.current.rootDir` 决定，常规安装版位于应用数据目录。

```text
<root>/
  storage.sqlite
  cache/
    <media_sha256>/
      base.vac
      chunks/
      tmp/
      mark_thumbnails/
        mark_<mark_id>_<render_digest>.png
```

- `storage.sqlite`: Flutter 侧持久索引数据库。
- `cache/<media_sha256>/`: 以媒体文件内容 SHA-256 分桶。analysis cache 和标注缩略图共享这个媒体桶。
- `mark_thumbnails/`: 标注列表预览用图片。它是可重建缓存，可以被 LRU 或用户手动清理。

无法读取本地文件内容时，媒体哈希会退化为 `media_id` 的 SHA-256；本地普通文件应始终使用文件内容 SHA-256。

## SQLite

数据库 schema 版本记录在 `schema_meta.schema_version`，当前为 `1`。标注 payload
和缩略图缓存也分别记录 `mark_payload_version`、`thumbnail_cache_version`，当前都为
`1`。

### `schema_meta`

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `key` | TEXT PRIMARY KEY | 元数据键 |
| `value` | TEXT NOT NULL | 元数据值 |

### `media`

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `hash` | TEXT PRIMARY KEY | 媒体内容 SHA-256 |
| `media_id` | TEXT NOT NULL | UI 侧稳定媒体 id，通常是规范化路径 |
| `path` | TEXT NOT NULL | 最近一次看到的路径 |
| `name` | TEXT NOT NULL | 最近一次看到的文件名 |
| `size` | INTEGER NOT NULL | 最近一次看到的文件大小 |
| `mtime_ms` | INTEGER NOT NULL | 最近一次看到的文件修改时间 |
| `first_seen_ms` | INTEGER NOT NULL | 首次登记时间 |
| `last_accessed_ms` | INTEGER NOT NULL | 最近访问时间 |

### `marks`

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `media_hash` | TEXT NOT NULL | 关联 `media.hash` |
| `mark_id` | INTEGER NOT NULL | 媒体内标注 id |
| `payload_json` | TEXT NOT NULL | 标注 payload |
| `updated_at_ms` | INTEGER NOT NULL | 最近更新时间 |

主键是 `(media_hash, mark_id)`。`payload_json.version` 当前为 `1`。

### `thumbnail_cache`

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `media_hash` | TEXT NOT NULL | 媒体内容 SHA-256 |
| `mark_id` | INTEGER NOT NULL | 标注 id |
| `render_digest` | TEXT NOT NULL | 缩略图输入状态摘要 |
| `path` | TEXT NOT NULL | 缩略图文件绝对路径 |
| `bytes` | INTEGER NOT NULL | 文件大小 |
| `updated_at_ms` | INTEGER NOT NULL | 最近生成时间 |
| `last_accessed_ms` | INTEGER NOT NULL | 最近访问时间 |
| `cache_version` | INTEGER NOT NULL | 缩略图缓存格式版本，当前为 `1` |

主键是 `(media_hash, mark_id, render_digest)`。

## 校验与清理

低频入口（例如设置页刷新、用户触发清理）会做轻量一致性校验：删除
`thumbnail_cache` 中已经找不到文件的记录。缩略图目录本身按文件系统扫描展示和
LRU 淘汰，删除文件后再由同一轻量校验收敛索引。

标注数据是用户数据，不走容量 LRU；标注缩略图和 analysis cache 都是可重建缓存。
