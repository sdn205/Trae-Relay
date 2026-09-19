# Trae Relay

当前版本：`0.1`

一个 Windows 原生的 Trae 本地 API 转接工具。读取当前 Windows 用户已登录的 Trae 账号，将 Trae 模型接入提供为 OpenAI 兼容的 HTTP 接口，供聊天客户端、脚本和开发工具调用, 无需安装 Python、Node.js 或 Docker，单exe即可使用。

目前仅测试使用trae code cn，trae的其他衍生版本（traework，或者国际版）请自行测试，不保证支持。

觉得好用的家人帮忙点个免费的star吧，球球啦

## 预览

![Trae Relay 运行界面](docs/images/%E9%A2%84%E8%A7%88.png)


## 功能

- **API 接入**：Chat Completions、Responses、模型列表，支持普通 JSON 响应与 SSE 流式输出。
- **工具调用**：函数工具声明、调用与结果回传；Responses 支持文本型自定义工具和命名空间工具。
- **多轮对话**：客户端提交完整历史，或通过 Responses 的 `previous_response_id` 延续本地缓存的会话。
- **模型设置**：读取账号可用模型目录，按模型调整思考档位与 Max 模式。
- **账号与记录**：发现本机登录账号、刷新令牌与积分、管理并发、查看使用记录。
- **桌面功能**：托盘常驻、开机自启动、自动签到，以及可选的局域网访问。

兼容范围见下文。不同模型、账号和 Trae 版本的可用能力可能不同。

## 使用环境

- Windows 10 / 11，x64。
- 已在**同一个 Windows 用户**下安装并登录 Trae，且账号可以正常使用目标模型。
- 可以连接 Trae 上游服务。
- 将程序放在当前用户可写的目录，配置、日志和使用记录默认保存在 EXE 旁。

目前实际验证主要针对 Trae CN。程序也会扫描 `%APPDATA%` 下的 `TRAE SOLO CN`、`Trae Work CN`、`Trae` 和 `TRAE SOLO` 登录目录；扫描到账号不代表对应版本的全部上游功能都已验证。

## 快速开始

1. 在 Trae 中完成登录，并确认可以正常发起对话。
2. 获取 `TraeRelay.exe`：若仓库已提供 Release，可下载其中的程序；也可以按下文从源码编译。
3. 将 EXE 放入独立目录后双击运行。首次运行会创建默认配置并生成本地 API Key，程序启动后自动监听 API 端口。
4. 在“运行总览”检查账号和模型是否加载成功，复制接口地址与密钥。
5. 在支持自定义 OpenAI 接口的客户端中填写：

| 配置项 | 填写内容 |
| --- | --- |
| 接口类型 | OpenAI 兼容；按客户端能力选择 Chat Completions 或 Responses |
| Base URL | `http://127.0.0.1:8317/v1` |
| API Key | “运行总览”中的本地密钥 |
| 模型 | `/v1/models` 返回的模型 `id` |

模型列表来自当前账号，请使用实际返回的 ID。填写模型名称本身不会获得账号尚未开放的模型权限。

部分客户端要求填写完整接口地址，此时分别使用 `http://127.0.0.1:8317/v1/chat/completions` 或 `http://127.0.0.1:8317/v1/responses`，避免重复拼接 `/v1`。

默认关闭窗口会最小化到托盘，API 继续运行。需要完全退出时，在托盘图标菜单中选择“退出”。

### 检查连接

在 PowerShell 中执行：

```powershell
# 健康检查无需密钥；返回 status=ok 仅代表本地服务已启动。
Invoke-RestMethod 'http://127.0.0.1:8317/health'

# 输入运行总览中的本地密钥，然后读取账号可用的模型。
$relayKey = Read-Host 'API Key'
$relayHeaders = @{ Authorization = "Bearer $relayKey" }
$relayModels = Invoke-RestMethod 'http://127.0.0.1:8317/v1/models' -Headers $relayHeaders
$relayModels.data | Select-Object id
```

默认启用 API Key 校验，支持 `Authorization: Bearer <key>` 和 `X-API-Key: <key>`。界面的“任意 Key”开关会跳过密钥校验，包括没有提供密钥的请求；开启局域网访问时应保留密钥校验。

## Python 接入示例

安装 [OpenAI Python SDK](https://developers.openai.com/api/docs/libraries)：

```powershell
python -m pip install -U openai
```

下面的代码连接本地 Trae Relay，使用本地生成的密钥：

```python
from getpass import getpass

from openai import OpenAI

client = OpenAI(
    base_url="http://127.0.0.1:8317/v1",
    api_key=getpass("Trae Relay API Key: "),
)

for item in client.models.list().data:
    print(item.id)

model = input("选择上面列出的模型 ID: ").strip()

completion = client.chat.completions.create(
    model=model,
    messages=[{"role": "user", "content": "用一句话介绍你自己。"}],
)
print(completion.choices[0].message.content)
```

以下示例接在上述代码后运行，复用 `client` 和 `model`。

### 流式输出

```python
stream = client.chat.completions.create(
    model=model,
    messages=[{"role": "user", "content": "解释什么是 HTTP 流式响应。"}],
    stream=True,
)

for chunk in stream:
    if chunk.choices:
        print(chunk.choices[0].delta.content or "", end="", flush=True)
print()
```

### Responses 与多轮对话

```python
first = client.responses.create(
    model=model,
    input="我正在学习 C++，请推荐一个入门练习。",
)
print(first.output_text)

second = client.responses.create(
    model=model,
    previous_response_id=first.id,
    input="把这个练习拆成三个步骤。",
)
print(second.output_text)
```

使用 `previous_response_id` 时，`input` 只提交新增内容。`instructions` 只作用于当前请求，需要持续使用时每轮重新传入。

## API 与兼容范围

### 已提供的接口

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| GET | `/health` | 本地服务健康检查，无需鉴权 |
| GET | `/v1/models` | 当前账号可用的模型列表 |
| POST | `/v1/chat/completions` | Chat Completions，支持流式与非流式 |
| POST | `/v1/responses` | Responses，支持流式与非流式 |
| GET | `/v1/status` | 本项目扩展接口，返回服务设置和账号状态 |
| OPTIONS | 上述 `/v1` 路径 | 浏览器跨域预检；实际 API 请求仍按配置鉴权 |

### 功能边界

| 能力 | 当前行为 |
| --- | --- |
| 文本、多轮对话 | 支持；Chat Completions 需由客户端提交历史 |
| 图片输入 | 转发 `image_url` / `input_image`，实际识图能力取决于上游模型；不提供图片生成接口 |
| 函数工具 | 支持原生工具调用与结果回传；工具由调用方执行，Relay 不执行客户端函数 |
| Responses 自定义工具 | 支持 `type: "custom"` 的文本格式和命名空间工具；`grammar` 格式返回 400 |
| `tool_choice` | `none` 会移除工具；指定名称会筛选可用工具，但不能保证模型一定调用；`required` 没有强制调用保证 |
| `parallel_tool_calls` | 保留上游返回的所有调用；设为 `false` 不保证上游仅返回一个调用 |
| 思考内容 | Chat Completions 使用扩展字段 `reasoning_content`；Responses 可映射为 reasoning summary |
| 思考档位 | 根据模型能力映射，通过系统提示前缀影响上游；不等同于原生推理预算控制 |
| `stop` | Chat Completions 在本地截断公开正文，最多 4 条；上游仍可能继续生成并消耗额度 |
| Token 用量 | 转换上游提供的 usage；Chat 流式请求可用 `stream_options.include_usage` 请求独立 usage 帧 |
| 结构化输出 | 未实现 `response_format` / `text.format` 的严格 JSON Schema 约束；请求成功不代表输出满足 schema |
| 生成参数 | 未实现 `temperature`、`top_p`、`seed`、`n`、`max_tokens`、`max_completion_tokens`、`max_output_tokens` 的对应控制；这些参数不能作为有效约束 |
| Responses 后台任务 | `background: true` 返回 400；没有响应查询、取消或删除端点 |
| Responses `store` | 不控制本地会话缓存策略；缓存行为由 `responses` 配置决定 |
| 内置搜索和托管工具 | 不提供 OpenAI 的 web search、file search、code interpreter 等托管执行环境；需要由客户端通过函数工具实现 |
| 文件、音频及其他接口 | 不支持 `input_file` / PDF 和音频输入；未识别内容块可能被忽略。未提供 Files、Embeddings、Audio、Images、Realtime、Batch 等接口 |

部分尚未实现的请求字段会被忽略。需要依赖严格参数语义的客户端，应先确认这里列出的兼容范围。

### Responses 会话缓存

默认在内存中保留最近 64 个响应对应的会话上下文。缓存被淘汰或程序重启后，原来的 `previous_response_id` 可能失效，并返回 400、错误码 `previous_response_not_found`。此时应重新发送完整上下文。

可以设置 `responses.sessionCachePersist: true` 将缓存写入 EXE 同目录的 `responses_sessions.json`。磁盘缓存保存文本与工具调用历史，**不保存图片内容**；需要完整恢复多模态上下文时，客户端应自行保留并重新提交图片。

## 配置

默认配置文件是 EXE 同目录的 `config.json`。建议先运行程序生成配置，再在界面中调整常用设置；手工编辑文件后重启程序。使用 `--config` 可以指定其他配置文件。

以下是可作为起点的最小配置，未填写的字段使用程序默认值：

```json
{
  "version": 1,
  "service": {
    "host": "127.0.0.1",
    "port": 8317,
    "allowLan": false,
    "apiKey": "",
    "allowAnyApiKey": false,
    "maxConcurrentPerAccount": 2
  },
  "responses": {
    "enabled": true,
    "sessionCacheSize": 64,
    "sessionCachePersist": false
  },
  "logging": {
    "level": "info",
    "retainDays": 7
  }
}
```

空 `apiKey` 会在加载时生成；需要固定密钥时可直接填写，或使用界面生成并保存。上面的 JSON 应使用 UTF-8 编码保存。

| 配置项 | 默认值 / 说明 |
| --- | --- |
| `service.port` | `8317` |
| `service.allowLan` | `false`；开启后监听 `0.0.0.0` |
| `service.allowAnyApiKey` | `false`；开启即跳过密钥校验 |
| `service.maxConcurrentPerAccount` | `2`，有效范围为 1～8 |
| `service.upstreamTimeoutSec` | `120` 秒 |
| `service.firstEventTimeoutSec` | `120` 秒 |
| `defaults.reasoningEffort` | 默认不覆盖；可用档位受模型能力限制 |
| `defaults.isMaxMode` | `0`；Max 模式需要上游模型与账号支持 |
| `accounts.checkin.enabled` | `true`；在图形界面或托盘模式下由定时器执行 |
| `accounts.checkin.hour` / `minute` | 本地时间 `10:00` |
| `logging.dir` | 空字符串表示 EXE 同目录的 `logs` |

仓库中的 `config.example.json` 还包含历史版本的 `channel`、`search` 等字段。当前使用固定上游通道，请以程序生成的配置和本文说明为准。

### 局域网使用

在“偏好设置”开启局域网访问后，其他设备的 Base URL 填写 `http://运行程序的电脑IP:8317/v1`，并使用同一份本地 API Key。还需允许对应端口通过 Windows 防火墙。

内置监听器使用 HTTP，不提供 HTTPS。请在可信网络中使用；浏览器还可能受 HTTPS 页面访问 HTTP 接口或本地网络权限的限制。

### 命令行

```powershell
# 显示主窗口，并自动启动 API 服务
.\TraeRelay.exe

# 启动后常驻托盘
.\TraeRelay.exe --tray

# 无图形界面运行 API 服务
.\TraeRelay.exe --serve

# 使用指定配置
.\TraeRelay.exe --config "C:\TraeRelay\config.json"

# 查看版本
.\TraeRelay.exe --version
```

`--serve` 是普通后台进程模式，不会注册为 Windows 系统服务，也不会运行图形界面的自动签到定时器。默认启用单实例；如已有实例运行，新启动的进程不会再建立一个独立监听服务。

## 从源码构建

安装以下工具：

- Visual Studio 2022 或 Build Tools 2022，选择“使用 C++ 的桌面开发”。
- MSVC v143 工具集及 Windows 10 / 11 SDK。
- CMake 3.20 或更新版本。

在仓库根目录执行：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel 4
```

输出为 `dist/TraeRelay.exe`。构建使用静态 MSVC 运行库（`/MT`）与 Windows 系统库，无需下载额外的第三方 C++ 依赖。

重新编译前请完全退出正在运行的 Trae Relay，避免 EXE 被占用。需要全量重建时，在确认当前目录为仓库根目录后执行：

```powershell
Remove-Item -LiteralPath .\build -Recurse
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel 4
```

### 代码目录

```text
src/
  main.cpp       程序入口与命令行
  app/           配置、服务生命周期、开机启动
  api/           OpenAI 兼容路由与 Responses 转换
  accounts/      账号发现、令牌刷新、积分与使用记录
  upstream/      Trae 模型目录、上游请求与事件解析
  window/        窗口、页面和托盘交互
  ui/            控件绘制、布局与 DPI 适配
  common/        HTTP、JSON、日志等基础模块
  res/           图标、清单及 Windows 资源
include/         对应头文件
docs/            项目技术记录
tools/           辅助开发工具
CMakeLists.txt   构建入口
```

## 常见问题

| 现象 | 排查方式 |
| --- | --- |
| 没有发现账号 | 确认当前 Windows 用户已登录 Trae，登录数据位于默认用户目录；登录后重新启动 Relay |
| 模型列表为空或请求提示模型不可用 | 先在 Trae 中确认账号权限和模型可用性，再检查 Relay 日志；模型目录随上游变化 |
| 401 / `invalid_api_key` | 使用 Relay 界面显示的本地密钥；重新生成后同步更新客户端 |
| 503 / 无可用账号或并发繁忙 | 检查账号登录、额度、冷却状态及同时进行的请求数量 |
| `previous_response_not_found` | 会话缓存已失效，重新发送完整上下文 |
| 服务启动失败 | 检查端口是否已被占用，或在配置中更换端口 |
| 本机可访问，其他设备无法访问 | 检查局域网开关、防火墙、电脑 IP 和客户端使用的端口 |
| 关闭窗口后服务仍在运行 | 默认关闭到托盘；使用托盘菜单中的“退出”结束进程 |

提交问题时，请附上 Windows 版本、Trae 版本、接口路径、模型 ID、是否流式以及去除密钥和个人内容后的最小请求与错误信息。

## 本地数据与发布

运行后，EXE 所在目录可能包含：

| 文件或目录 | 内容 |
| --- | --- |
| `config.json` | 本地配置和 API Key |
| `logs/` | 运行日志 |
| `usage/` | 按日期保存的使用记录 |
| `responses_sessions.json` | 开启持久化后保存的文本与工具调用历史 |

发布程序时只分发构建好的 EXE 和公开说明即可，让使用者在自己的电脑上生成配置。上传源码时应排除 `build/`、本机运行产生的 `dist/` 数据、日志、会话缓存、私人截图和账号资料；不要将自己的运行目录整体作为发布包。
