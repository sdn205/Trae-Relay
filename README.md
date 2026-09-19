# Trae Relay

当前版本：`0.1`

Windows 原生的 Trae 本地 API 转接工具，将当前用户已登录的 Trae 账号提供为 OpenAI 兼容接口。单个 EXE 即可运行，无需 Python、Node.js 或 Docker。

目前主要验证 Trae CN，Trae Work 和国际版请自行测试。

欢迎提交 Issue 或 Star。

## 预览

![Trae Relay 运行界面](docs/images/%E9%A2%84%E8%A7%88.png)

## 功能

- Chat Completions、Responses、模型列表
- JSON 和 SSE 流式输出
- 函数工具、Responses 自定义工具、命名空间工具
- 多轮会话、思考档位、Max 模式
- 账号发现、令牌刷新、积分和使用记录
- 托盘、开机自启动、自动签到、局域网访问

## 使用环境

- Windows 10/11 x64
- 当前 Windows 用户已安装并登录 Trae
- 能够连接 Trae 上游服务

## 快速开始

1. 登录 Trae，并确认可以正常对话。
2. 下载 Release 中的 `TraeRelay.exe`，或按“从源码构建”编译。
3. 双击运行。首次启动会在 EXE 同目录生成 `config.json` 和 API Key，并监听 `8317` 端口。
4. 在“运行总览”确认账号和模型已加载，然后在客户端填写：

| 配置项 | 内容 |
| --- | --- |
| Base URL | `http://127.0.0.1:8317/v1` |
| API Key | “运行总览”中的本地密钥 |
| 模型 | `/v1/models` 返回的 `id` |

默认关闭窗口会最小化到托盘；完全退出请在托盘菜单中选择“退出”。

健康检查：`GET http://127.0.0.1:8317/health`，无需密钥。

API 支持 `Authorization: Bearer <key>` 和 `X-API-Key: <key>`。开启“任意 Key”会跳过鉴权，局域网使用时不建议开启。

## API

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/health` | 健康检查，无需鉴权 |
| GET | `/v1/models` | 当前账号可用模型 |
| POST | `/v1/chat/completions` | Chat Completions |
| POST | `/v1/responses` | Responses |
| GET | `/v1/status` | 服务和账号状态 |
| OPTIONS | `/v1/*` | 浏览器跨域预检 |

## 兼容范围

- 支持文本、多轮对话、图片输入（能力取决于上游模型）、工具调用和 SSE。
- Chat Completions 支持 `reasoning_content`；Responses 支持 reasoning summary 和 `previous_response_id`。
- `stop` 由本地截断正文，最多 4 条；上游仍可能继续消耗额度。
- `previous_response_id` 默认只保留最近 64 个响应，重启或淘汰后会返回 `previous_response_not_found`。
- 可通过 `responses.sessionCachePersist` 持久化文本和工具历史，图片不会保存。

以下功能当前不提供或不保证语义一致：

- 严格 JSON Schema 结构化输出
- `temperature`、`top_p`、`seed`、`n`、`max_tokens`、`max_completion_tokens`、`max_output_tokens` 控制
- `background` 后台任务及响应查询、取消接口
- OpenAI 托管的 web search、file search、code interpreter
- `input_file`/PDF、音频、Files、Embeddings、Images、Realtime、Batch 等接口

部分未知字段会被忽略；依赖严格参数语义的客户端请先验证。

## 配置

配置文件：EXE 同目录的 `config.json`。首次运行自动生成，也可以使用 `--config` 指定路径。

| 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `service.port` | `8317` | HTTP 端口 |
| `service.allowLan` | `false` | 开启后监听 `0.0.0.0` |
| `service.allowAnyApiKey` | `false` | 跳过 API Key 校验 |
| `service.maxConcurrentPerAccount` | `2` | 每账号并发数，范围 1～8 |
| `responses.sessionCacheSize` | `64` | Responses 会话缓存数量 |
| `responses.sessionCachePersist` | `false` | 是否保存会话到磁盘 |
| `logging.level` | `info` | 日志级别 |

开启局域网访问后，其他设备使用 `http://电脑IP:8317/v1`，并放行 Windows 防火墙端口。服务只提供 HTTP，请在可信网络中使用。

## 命令行

```powershell
.\TraeRelay.exe                 # 启动图形界面和 API
.\TraeRelay.exe --tray         # 启动后常驻托盘
.\TraeRelay.exe --serve        # 无图形界面运行 API
.\TraeRelay.exe --config path  # 指定配置文件
.\TraeRelay.exe --version      # 查看版本
```

`--serve` 不是 Windows 系统服务；默认启用单实例。

## 从源码构建

需要 Visual Studio 2022（桌面 C++）、Windows SDK 和 CMake 3.20+。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel 4
```

产物：`dist/TraeRelay.exe`。全量重建：

```powershell
Remove-Item -LiteralPath .\build -Recurse
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel 4
```

## 仓库内容

```text
src/                    C++ 源码、资源和版本模板
include/                头文件
docs/images/预览.png    软件界面预览图
CMakeLists.txt          CMake 构建配置
config.example.json     配置示例
README.md               项目说明
```

## 常见问题

| 现象 | 处理方式 |
| --- | --- |
| 没有发现账号 | 确认当前用户已登录 Trae，然后重启 Relay |
| 模型不可用 | 确认 Trae 账号权限和模型 ID，并查看 `logs/` |
| 401 | 使用“运行总览”中的 API Key |
| 503 | 检查账号、额度、冷却和并发数 |
| 端口启动失败 | 更换端口或关闭占用端口的程序 |
| 关闭窗口后仍在运行 | 程序默认最小化到托盘，在托盘菜单选择“退出” |

## 本地数据

| 路径 | 内容 |
| --- | --- |
| `config.json` | 配置和 API Key |
| `logs/` | 日志 |
| `usage/` | 使用记录 |
| `responses_sessions.json` | 开启持久化后的会话缓存 |

发布时只需分发 EXE 和 README；不要上传自己的配置、日志、会话缓存或账号资料。
