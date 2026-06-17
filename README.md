# ReliableMessageDelivery

ReliableMessageDelivery 是一个可靠消息投递原型项目。核心后端使用 C++11 实现，重点验证即时通信场景里的消息可靠性：在线投递、离线暂存、上线补发、ACK、超时重试、幂等去重和消息状态追踪。

当前项目已经新增 `web_gateway/`，用于提供浏览器聊天演示界面。浏览器不直接连接 C++ IMServer，而是先连接 Web Gateway，再由 Gateway 使用现有 TCP/Protobuf 协议连接 IMServer。

## 当前架构

```text
Browser
  | HTTP: GET /, POST /api/login, POST /api/register
  | WebSocket: /ws?token=...
  v
Web Gateway (Node.js + TypeScript)
  | TCP + Protobuf Envelope
  v
C++ IMServer
  | Redis command
  v
Redis MessageStore
```

默认端口：

```text
Web Gateway: 127.0.0.1:3000
IMServer:    0.0.0.0:8080
Redis:       127.0.0.1:6379
```

第一版 Web Gateway 采用兼容现有 IMServer 的模型：

```text
1 个浏览器 WebSocket 用户
  -> 1 个 Gateway Session
  -> 1 条到 IMServer 的 TCP/Protobuf 连接
```

因此 IMServer 暂时不需要改造，仍然保持 `uid -> TCP connection` 的在线用户模型。

## 目录结构

```text
client/          C++ 压测/演示客户端
server/          C++ IMServer
protocol/        Protobuf 协议和编解码辅助代码
web_gateway/     浏览器聊天演示网关
records/         阶段设计和开发记录
CMakeLists.txt   C++ 工程入口
README.md        项目说明
```

## 已实现能力

C++ IMServer 已实现：

- 基于 Muduo 的 TCP 长连接服务。
- Protobuf `Envelope` 协议封装。
- `4 字节网络序长度头 + Protobuf body` 的 TCP 帧格式。
- 客户端登录绑定 uid。
- 在线用户状态管理：`uid -> TcpConnectionPtr`、`conn -> uid`。
- 在线消息实时投递。
- 离线消息保存为 Redis Pending。
- 用户重新登录后按 Pending 索引补发消息。
- 发送方 ACK：服务端收到并处理 `CHAT_REQ` 后返回 ACK。
- 接收方 ACK：客户端收到 `CHAT_PUSH` 后返回 ACK。
- 服务端收到接收方 ACK 后将消息标记为 `Acked`。
- `Pending`、`Delivered`、`Acked`、`Failed` 消息状态流转。
- 客户端发送侧 `pending_acks` 和超时重试。
- 服务端 `Delivered` 超时扫描和重投。
- Redis 版 `MessageStore`。
- 基于 `from_uid + client_msg_id` 的服务端持久化幂等去重。

Web Gateway 已实现：

- 静态前端页面托管：`GET /`。
- 登录接口：`POST /api/login`。
- 注册接口：`POST /api/register`。
- WebSocket 长连接入口：`/ws?token=...`。
- 演示版认证：账号等于密码即成功，例如 `1 / 1`。
- 内存 token 管理。
- WebSocket session 管理。
- 浏览器 JSON 消息和 IMServer Protobuf 消息互转。
- 一个 Web 用户对应一条 IMServer TCP 连接。
- 发送消息、发送方 ACK、接收消息、接收方 ACK 的完整转发链路。

## 快速开始

### 1. 构建 C++ 后端

```bash
cmake -S . -B build
cmake --build build
```

### 2. 启动 Redis

IMServer 默认连接本机 Redis：

```text
127.0.0.1:6379
```

可以用下面命令确认 Redis 是否在监听：

```bash
ss -ltnp | grep 6379
```

### 3. 启动 IMServer

```bash
./bin/server
```

IMServer 默认监听：

```text
0.0.0.0:8080
```

### 4. 启动 Web Gateway

```bash
cd web_gateway
npm install
npm run dev
```

默认访问地址：

```text
http://127.0.0.1:3000/
```

如果需要让虚拟机外部或局域网访问 Gateway，可以改监听地址：

```bash
WEB_GATEWAY_HOST=0.0.0.0 npm run dev
```

也可以指定 IMServer 地址：

```bash
IM_HOST=127.0.0.1 IM_PORT=8080 npm run dev
```

### 5. 浏览器演示

打开两个浏览器窗口，分别登录：

```text
用户 1：账号 1，密码 1
用户 2：账号 2，密码 2
```

登录成功后进入聊天界面，在左侧选择或输入目标 uid，即可发送消息。

## Web Gateway 接口

### 登录

```http
POST /api/login
Content-Type: application/json

{
  "user_id": 1,
  "password": "1"
}
```

成功响应：

```json
{
  "ok": true,
  "uid": 1,
  "token": "..."
}
```

### 注册

```http
POST /api/register
Content-Type: application/json

{
  "user_id": 2,
  "password": "2"
}
```

成功响应：

```json
{
  "ok": true,
  "uid": 2
}
```

### WebSocket

连接地址：

```text
/ws?token=登录返回的 token
```

浏览器发送消息：

```json
{
  "type": "send_message",
  "to_uid": 2,
  "content": "hello",
  "client_msg_id": "1-xxx"
}
```

Gateway 返回发送方 ACK：

```json
{
  "type": "send_ack",
  "ok": true,
  "client_msg_id": "1-xxx",
  "msg_id": 512002
}
```

Gateway 下发聊天消息：

```json
{
  "type": "message",
  "msg_id": 512002,
  "from_uid": 1,
  "to_uid": 2,
  "content": "hello",
  "server_timestamp_ms": 1781692426459
}
```

浏览器确认收到消息：

```json
{
  "type": "message_ack",
  "msg_id": 512002
}
```

## IMServer 协议

协议定义位于：

```text
protocol/Message.proto
```

核心消息类型：

```text
LOGIN_REQ
LOGIN_RESP
CHAT_REQ
CHAT_PUSH
ACK
HEARTBEAT
```

所有业务消息都包在 `Envelope` 中：

```protobuf
message Envelope {
  MessageType type = 1;
  uint64 seq = 2;
  uint64 timestamp_ms = 3;

  oneof payload {
    LoginRequest  login_req = 10;
    LoginResponse login_resp = 11;
    ChatRequest   chat_req = 12;
    ChatPush      chat_push = 13;
    Ack           ack = 14;
    Heartbeat     heartbeat = 15;
  }
}
```

TCP 传输格式：

```text
4 字节大端 body 长度 + 序列化后的 message::Envelope
```

## 可靠投递流程

### 在线投递

```text
发送方
  -> CHAT_REQ(from_uid, to_uid, content, client_msg_id)

服务端
  -> 校验连接已登录
  -> 校验 from_uid 和连接 uid 一致
  -> MessageStore::createMessage()
  -> 根据 from_uid + client_msg_id 幂等去重
  -> 目标在线则发送 CHAT_PUSH
  -> markDelivered(msg_id)
  -> 给发送方返回 ACK(msg_id)

接收方
  -> 收到 CHAT_PUSH
  -> 返回 ACK(msg_id)

服务端
  -> 校验 ACK 来自消息接收方
  -> markAcked(msg_id)
```

### 离线补发

```text
发送方发消息
  -> 目标用户不在线
  -> MessageStore 将消息保存为 Redis Pending
  -> 服务端仍给发送方 ACK，表示消息已被服务端可靠接收

目标用户重新登录
  -> handleLogin()
  -> deliverPendingMessages(uid)
  -> tryDeliverMessage(msg_id)
  -> CHAT_PUSH
  -> Delivered
  -> 接收方 ACK
  -> Acked
```

### 超时重投

服务端后台 `deliveryLoop()` 会定期扫描：

```text
Delivered 且超过 ACK 等待时间的消息
```

未超过最大重试次数时会重新投递，超过最大重试次数后会标记为 `Failed`。

## 常用验证命令

查看端口：

```bash
ss -ltnp | grep 3000
ss -ltnp | grep 8080
ss -ltnp | grep 6379
```

构建 Web Gateway：

```bash
cd web_gateway
npm run build
```

运行 C++ 压测客户端：

```bash
./bin/client 16 1000 quiet 60
```

## 当前限制

- Web Gateway 认证仍是演示版，账号等于密码。
- 用户、token 都保存在 Gateway 内存中，服务重启会丢失。
- 当前没有数据库用户表。
- 当前没有浏览器历史消息查询接口。
- Gateway 到 IMServer 还没有断线自动重连和退避策略。
- 当前是一个 Web 用户对应一条 IMServer TCP 连接，还没有做 Gateway 到 IMServer 的多路复用。
- 多 Gateway 部署时还没有用户路由表。
- 还没有 TLS、反向代理、限流、生产级日志和 `/metrics`。

## 后续方向

优先建议：

1. 补齐 WebSocket 心跳、断线重连和前端连接状态提示。
2. 增加消息发送中、已确认、失败等 UI 状态。
3. 增加历史消息查询接口。
4. 将 `AuthService` 从内存演示版替换为数据库用户表。
5. 增加 Gateway 联调测试。

等浏览器演示链路稳定后，再考虑是否改造 IMServer 的连接模型，从 `uid -> TCP connection` 演进为 `uid -> gateway route`，或设计 Gateway 到 IMServer 的内部多路复用协议。
