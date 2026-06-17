# 2026-06-17 Web Gateway 阶段进展

## 背景

本阶段目标是在现有 C++ IMServer 不大改的前提下，新增一个 Web Gateway，让用户可以直接通过浏览器完成登录、注册和聊天演示。

现有 IMServer 的连接模型仍然是：

```text
uid -> TCP connection
connection -> uid
```

因此第一版网关选择兼容现有模型：

```text
Browser WebSocket 用户 1 个
  -> Web Gateway 内部 Session 1 个
  -> IMServer TCP/Protobuf 连接 1 条
```

这不是最终最高并发形态，但它最小、清晰、风险低，适合先把浏览器聊天链路跑通。

## 已确定的设计

1. 浏览器只连接 Web Gateway，不直接连接 C++ IMServer。
2. Web Gateway 提供 HTTP 登录/注册接口，也提供 `/ws` WebSocket 长连接。
3. Web Gateway 负责认证、浏览器 Session 管理、WebSocket 收发和协议转换。
4. IMServer 暂时不改，继续认为每条 TCP 连接对应一个登录 uid。
5. Gateway 到 IMServer 使用现有协议：4 字节大端长度头 + Protobuf `Envelope`。
6. Browser 到 Gateway 使用 JSON，便于前端开发和调试。
7. 第一版认证规则很简单：账号等于密码即认证成功，例如 `1 / 1`、`2 / 2`。
8. token 由 `AuthService` 生成，用于浏览器登录后建立 WebSocket 时证明身份。
9. session_id 表示一次具体的 WebSocket 在线连接，和 uid 不是同一个概念。

## 已实现内容

### 前端界面

位置：`web_gateway/public/`

已实现：

- 登录/注册切换界面。
- 登录界面左侧为登录表单，右侧为注册入口。
- 点击注册后，左侧变为登录入口，右侧变为注册表单。
- 登录成功后进入聊天界面。
- 聊天界面左侧为会话列表和目标 uid 输入。
- 聊天界面右侧为消息区域和输入框。
- 前端调用：
  - `POST /api/login`
  - `POST /api/register`
  - `WebSocket /ws?token=...`

### HTTP 入口

位置：`web_gateway/src/server.ts`

职责：

- `GET /`：返回静态前端页面。
- `POST /api/login`：登录，返回 uid 和 token。
- `POST /api/register`：注册演示账号。
- `WebSocket /ws`：通过 HTTP Upgrade 进入 WebSocket 长连接。
- 加载 `protocol/Message.proto`。
- 组合 `AuthService`、`WsSessionManager`、`GatewayWebSocketServer`。

默认配置：

```text
WEB_GATEWAY_HOST=127.0.0.1
WEB_GATEWAY_PORT=3000
IM_HOST=127.0.0.1
IM_PORT=8080
TOKEN_TTL_MS=7200000
```

### 认证模块

位置：`web_gateway/src/AuthService.ts`

当前能力：

- 内存保存用户。
- 内存保存 token。
- token 有过期时间。
- 登录时如果账号尚未注册，但满足账号等于密码，会自动加入内存用户表。
- 注册时要求账号为正整数，密码非空，且账号等于密码。

当前限制：

- 没有数据库。
- 没有密码哈希。
- 没有刷新 token。
- 服务重启后用户和 token 都会丢失。

### WebSocket Session 管理

位置：`web_gateway/src/WsSessionManager.ts`

当前能力：

- `WsSession` 和 `WsSessionManager` 已合并到一个文件。
- `WsSession` 表示一次浏览器 WebSocket 连接。
- `WsSessionManager` 维护：
  - `uid -> session`
  - `session_id -> session`
- 同一个 uid 新连接上线时，会替换旧 session。
- 删除 session 时会检查 session_id，避免旧连接关闭误删新连接。

### WebSocket 网关模块

位置：`web_gateway/src/WebSocketServer.ts`

当前能力：

- 接收 `/ws?token=...`。
- 调用 `AuthService.verifyToken()` 校验 token。
- token 有效后创建 WebSocket session。
- 为该 session 创建一个 `ImClient`。
- `ImClient` 登录 IMServer 成功后，才通知浏览器 `online`。
- 浏览器发送 `send_message` 时，网关转换为 IMServer `CHAT_REQ`。
- IMServer 返回 `ACK` 时，网关转换为浏览器 `send_ack`。
- IMServer 下发 `CHAT_PUSH` 时，网关转换为浏览器 `message`。
- 浏览器发送 `message_ack` 时，网关转换为 IMServer `ACK`。
- WebSocket 关闭时，同时关闭对应 IMServer TCP 连接。

### IMServer TCP 客户端模块

位置：

- `web_gateway/src/ImProtocol.ts`
- `web_gateway/src/ImClient.ts`

当前能力：

- 读取 `protocol/Message.proto`。
- 编解码现有 IMServer Protobuf 协议。
- 实现 4 字节大端长度头的 TCP 拆包/组包。
- Gateway 作为 IMServer 客户端登录。
- 发送 `LOGIN_REQ`。
- 发送 `CHAT_REQ`。
- 发送接收方 `ACK`。
- 接收 `LOGIN_RESP`、`ACK`、`CHAT_PUSH`。
- 维护发送 seq 到 `client_msg_id` 的映射，用于把 IMServer ACK 对应回浏览器消息。
- 维护 `msg_id -> push seq`，用于浏览器 ack 某条下发消息时，向 IMServer 回正确的 ACK。

## 当前数据流程

### 登录流程

```text
Browser
  -> POST /api/login { user_id, password }

Web Gateway
  -> AuthService.login()
  -> 返回 { ok, uid, token }
```

### WebSocket 上线流程

```text
Browser
  -> WebSocket /ws?token=xxx

Web Gateway
  -> verifyToken(token)
  -> 创建 WsSession
  -> 创建 ImClient
  -> TCP 连接 IMServer
  -> LOGIN_REQ(uid)

IMServer
  -> LOGIN_RESP(ok=true)

Web Gateway
  -> Browser: { type: "online", uid, session_id }
```

### 发送消息流程

```text
Browser A
  -> Gateway: send_message(to_uid, content, client_msg_id)

Gateway A
  -> IMServer: CHAT_REQ(from_uid, to_uid, content, client_msg_id)

IMServer
  -> 存储消息
  -> 返回 ACK 给发送方连接
  -> 如果目标在线，发送 CHAT_PUSH 给目标连接

Gateway A
  -> Browser A: send_ack(ok, msg_id, client_msg_id)

Gateway B
  -> Browser B: message(msg_id, from_uid, to_uid, content)

Browser B
  -> Gateway B: message_ack(msg_id)

Gateway B
  -> IMServer: ACK(msg_id)
```

## 已验证结果

已通过 TypeScript 构建：

```bash
cd web_gateway
npm run build
```

已通过真实 IMServer 联调验证：

```text
online 9301 9302
send_ack true 512002
message 9301 9302 imserver-integration-1781692426459
```

含义：

- 两个浏览器用户通过 Gateway 成功上线。
- 发送方收到 IMServer ACK。
- 接收方收到 IMServer CHAT_PUSH。
- Gateway 到 IMServer 的 Protobuf/TCP 链路可用。

## 当前限制

1. 认证仍是演示版，账号等于密码。
2. 用户和 token 都保存在内存中，服务重启会丢失。
3. 当前是一个 Web 用户对应一条 IMServer TCP 连接。
4. Gateway 还没有做 IMServer 断线自动重连。
5. 浏览器端没有历史消息 API。
6. 没有多 Gateway 部署下的路由表。
7. 没有 TLS、反向代理、限流、指标和生产级日志。
8. IMServer 仍然是 `uid -> TCP connection` 模型，尚未演进为 `uid -> gateway route` 模型。

## 后续建议

短期建议：

1. 保持 IMServer 不改，继续完善 Gateway 演示链路。
2. 增加 WebSocket 心跳、断线提示和重连。
3. 增加浏览器端消息发送中、已送达、失败状态。
4. 增加简单历史消息查询接口。
5. 增加 Gateway 端单元测试和最小联调脚本。

中期建议：

1. 认证接入数据库，密码改为哈希保存。
2. token 改成 JWT 或服务端 session 存储。
3. 增加 Gateway 到 IMServer 的连接重试和退避。
4. 增加 `/metrics` 和结构化日志。
5. 再考虑 IMServer 是否要从 `uid -> conn` 演进为 `uid -> route/gateway`。

长期建议：

1. 多 Gateway 部署时，需要用户路由表或网关注册表。
2. IMServer 可以逐步拆分为连接层、消息存储层、投递调度层。
3. 当 Gateway 需要一条 TCP 连接承载多个 uid 时，再设计 Gateway 与 IMServer 之间的内部多路复用协议。
4. 到那个阶段，IMServer 才会更接近消息中间件或投递中心。
