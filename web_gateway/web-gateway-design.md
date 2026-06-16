# Web Gateway 最小可实现设计

## 目标

在当前 IMServer 前增加一个 Web Gateway，让浏览器可以通过网页聊天，同时保持 IMServer 继续负责消息可靠投递。

最小闭环：

```text
Browser
  -- HTTP / WebSocket -->
Web Gateway
  -- TCP + Protobuf -->
IMServer
  -- Redis -->
MessageStore
```

Gateway 负责 Web 接入，IMServer 负责消息业务。

## 技术方案

推荐：

```text
Web Gateway:
  Node.js + TypeScript + Fastify + ws

内部协议:
  protobufjs 读取 ../protocol/Message.proto
  Node net.Socket 连接 IMServer

前端:
  原生 HTML + CSS + JavaScript
```

TypeScript 不是 Java。它是 JavaScript 的类型增强版本，开发时写 `.ts`，编译后仍然是 JavaScript，并运行在 Node.js 上；它不运行在 JVM 上，也不依赖 Java 语言体系。

选择理由：

- Fastify 处理 HTTP API 和静态页面简单。
- ws 处理 WebSocket 足够轻量。
- Node 的 `net.Socket` 可以直接连接当前 C++ TCP 服务。
- protobufjs 可以复用现有 `Message.proto`，减少重复协议定义。
- Gateway 是 Web/BFF 层，用 TypeScript 更适合后续扩展登录、鉴权、前端接口。

nginx 暂时不作为业务网关。以后需要 TLS、反向代理、负载均衡时再放在 Gateway 前面。

## 目录结构


第一版可以只实现必要文件：

```text
src/server.ts
src/AuthService.ts
src/WsSessionManager.ts
src/ImClient.ts
src/PendingDeliveryStore.ts
public/index.html
public/app.js
public/style.css
```

等代码变大后再拆成上面的完整目录。

## 模块职责

### 浏览器请求入口

浏览器发来的请求由两个模块接收：

```text
普通 HTTP 请求:
  HttpApi 接收
  例如 GET /、POST /api/login、GET /api/me

实时 WebSocket 请求:
  WebSocketServer 接收
  例如 /ws?token=xxx、send_message、message_ack、ping
```

`ImClient` 和 `ImConnection` 不直接接收浏览器请求，它们只负责 Gateway 与 IMServer 之间的内部 TCP + Protobuf 通信。

### HttpApi

负责 HTTP 能力：

```text
GET  /
POST /api/login
GET  /api/me
```

第一版 `POST /api/login` 接收用户 id 和密码：

```json
{"user_id": 1, "password": "rmd-demo"}
```

返回 token：

```json
{"ok": true, "token": "demo-token", "uid": 1}
```

注意：

```text
HTTP 登录成功只表示身份认证成功，不表示 IM 在线。
WebSocket 连接成功并完成 USER_ONLINE 后，才表示聊天意义上的在线。
```

### AuthService

负责：

```text
校验 user_id + password
生成 token
解析 token
校验 WebSocket 连接身份
```

第一版先使用默认密码，不接数据库：

```text
默认密码: rmd-demo
```

认证规则：

```text
user_id 必须是正整数
password 必须等于默认密码
```

第一版用户和 token 都放内存：

```text
user_id -> demo user
token -> uid
```

后续接数据库时，只替换 `AuthService` 内部实现：

```text
AuthService
  -> UserRepository
  -> MySQL/PostgreSQL/Redis
```

对 `HttpApi`、`WebSocketServer`、`WsSessionManager` 和 IMServer 内部协议不产生结构性影响。

### WebSocketServer

负责浏览器实时连接：

```text
WebSocket /ws?token=xxx
```

连接成功后：

1. 校验 token。
2. 创建 `WsSession`。
3. 通知 IMServer 用户上线。
4. 收到 IMServer 确认后向浏览器发送 `online`。

### WsSessionManager

维护浏览器 session：

```text
uid -> WsSession
session_id -> WsSession
```

第一版只支持单端在线：

```text
同一个 uid 新连接上线时，关闭旧 session
```

后续多端登录时改成：

```text
uid -> session_id 列表
```

### ImClient

负责 Gateway 与 IMServer 的内部通信：

```text
connect()
registerGateway()
userOnline(uid, session_id)
userOffline(uid, session_id)
sendMessage(from_uid, to_uid, content, client_msg_id)
ackMessage(uid, session_id, msg_id)
```

第一版可以使用一条 TCP 长连接。后续压力变大后再演进为连接池：

```text
按 uid hash 到不同内部连接
```

### ImConnection

负责底层 TCP 帧：

```text
4 字节网络序长度头 + Protobuf body
```

职责：

```text
连接 IMServer
重连
发送完整 frame
接收半包/粘包
解析 Protobuf Envelope
```

### PendingDeliveryStore

负责 Gateway 本地暂存未被浏览器 ACK 的投递消息：

```text
msg_id -> PendingDelivery
```

第一版用内存：

```ts
type PendingDelivery = {
  msgId: number;
  uid: number;
  sessionId: string;
  fromUid: number;
  content: string;
  createdAtMs: number;
};
```

Gateway 收到 IMServer 的投递后先暂存，推给浏览器。浏览器 ACK 后，Gateway 再 ACK 给 IMServer，然后删除暂存。

## ACK 语义

必须区分两类 ACK。

### 发送方 ACK

语义：

```text
IMServer 已经接收并持久化发送方消息
```

流程：

```text
Browser A -> Gateway: send_message
Gateway -> IMServer: SEND_MESSAGE
IMServer -> Gateway: SEND_ACK
Gateway -> Browser A: send_ack
```

这不代表接收方已经收到。

### 接收方 ACK

语义：

```text
目标浏览器已经收到消息
```

流程：

```text
IMServer -> Gateway: DELIVER_MESSAGE
Gateway -> Browser B: message
Browser B -> Gateway: message_ack
Gateway -> IMServer: USER_ACK
IMServer: 标记 Acked
```

Gateway 不能在浏览器 ACK 前提前向 IMServer 发送接收方 ACK。否则 Gateway 崩溃或浏览器断开时，消息可能丢失。

## 浏览器 WebSocket 协议

### Gateway -> Browser

上线成功：

```json
{"type":"online","uid":1,"session_id":"s-xxx"}
```

发送结果：

```json
{"type":"send_ack","ok":true,"client_msg_id":"c-1","msg_id":1001}
```

收到消息：

```json
{
  "type":"message",
  "msg_id":1001,
  "from_uid":1,
  "to_uid":2,
  "content":"hello",
  "server_timestamp_ms":1710000000000
}
```

错误：

```json
{"type":"error","reason":"message server unavailable"}
```

### Browser -> Gateway

发送消息：

```json
{
  "type":"send_message",
  "to_uid":2,
  "content":"hello",
  "client_msg_id":"browser-generated-id"
}
```

确认收到：

```json
{"type":"message_ack","msg_id":1001}
```

心跳：

```json
{"type":"ping"}
```

## Gateway 与 IMServer 内部协议

当前 IMServer 是“一条连接绑定一个 uid”的模型。为了让 Gateway 一条连接承载多个用户，IMServer 需要演进成“一条连接绑定一个 gateway”的模型。

连接身份：

```text
旧模型:
  conn -> uid
  uid -> conn

新模型:
  conn -> gateway_id
  uid -> { gateway_id, session_id }
```

建议在 Protobuf 中增加内部消息类型：

```text
GATEWAY_REGISTER
GATEWAY_REGISTER_RESP
USER_ONLINE
USER_OFFLINE
SEND_MESSAGE
SEND_ACK
DELIVER_MESSAGE
USER_ACK
```

最小字段：

```text
gateway_id
session_id
from_uid
to_uid
client_msg_id
msg_id
content
ok
reason
timestamp_ms
```

IMServer 必须校验：

```text
SEND_MESSAGE.from_uid 属于当前 gateway/session
USER_ACK.uid 是 msg_id 的接收方
USER_ACK.uid 属于当前 gateway/session
```

## 关键流程

### 登录和上线

```text
Browser -> Gateway:
POST /api/login {user_id, password}

Gateway:
AuthService 校验 user_id + password

Gateway -> Browser:
token, uid

Browser -> Gateway:
WebSocket /ws?token=xxx

Gateway:
校验 token
生成 session_id

Gateway -> IMServer:
USER_ONLINE(uid, gateway_id, session_id)

IMServer:
记录 uid -> gateway_id/session_id
投递该 uid 的 Pending 消息

Gateway -> Browser:
online
```

### 发送在线消息

```text
Browser A -> Gateway:
send_message(to_uid=2, content)

Gateway:
根据 token/session 得到 from_uid=1

Gateway -> IMServer:
SEND_MESSAGE(from_uid=1, to_uid=2, content, client_msg_id)

IMServer:
幂等去重
存储消息
分配 msg_id
返回 SEND_ACK 给发送方 Gateway
查找 to_uid=2 的 route
发送 DELIVER_MESSAGE 给接收方 Gateway

Gateway -> Browser A:
send_ack

Gateway -> Browser B:
message

Browser B -> Gateway:
message_ack

Gateway -> IMServer:
USER_ACK(uid=2, msg_id)

IMServer:
markAcked(msg_id)
```

### 发送离线消息

```text
Browser A -> Gateway:
send_message(to_uid=2)

Gateway -> IMServer:
SEND_MESSAGE

IMServer:
存储消息
返回 SEND_ACK
发现 to_uid=2 不在线
保持 Pending

Browser B 上线:
Gateway -> IMServer: USER_ONLINE(uid=2)
IMServer -> Gateway: DELIVER_MESSAGE(pending msg)
Gateway -> Browser B: message
Browser B -> Gateway: message_ack
Gateway -> IMServer: USER_ACK
```

### 断开连接

```text
Browser WebSocket closed

Gateway:
删除 WsSession
删除该 session 的本地 pending delivery

Gateway -> IMServer:
USER_OFFLINE(uid, session_id)

IMServer:
删除 uid route
后续消息进入 Pending
```

如果 Gateway 已经收到 `DELIVER_MESSAGE`，但浏览器还没有 ACK：

```text
Gateway 不发送 USER_ACK
IMServer 保持 Delivered/Pending 状态，后续重试或上线补发
```

## 最小实现步骤

### 第一步：Gateway 静态页面和登录

实现：

```text
GET /
POST /api/login
WebSocket /ws?token=xxx
```

验证：

```text
打开页面
输入 user_id 和 password
密码正确时返回 token
登录后 WebSocket 连接成功
页面显示 online
```

### 第二步：Gateway 连接 IMServer

实现：

```text
ImConnection
Protobuf 编解码
GATEWAY_REGISTER
USER_ONLINE
USER_OFFLINE
```

验证：

```text
浏览器上线后，IMServer 能记录 uid route
浏览器关闭后，IMServer 能删除 uid route
```

### 第三步：发送消息

实现：

```text
Browser send_message
Gateway SEND_MESSAGE
IMServer 存储消息
IMServer SEND_ACK
Gateway send_ack
```

验证：

```text
uid=1 发消息给 uid=2
uid=1 页面收到 send_ack
Redis/日志中能看到消息已存储
```

### 第四步：在线投递和 ACK

实现：

```text
IMServer DELIVER_MESSAGE
Gateway message
Browser message_ack
Gateway USER_ACK
IMServer markAcked
```

验证：

```text
uid=1 和 uid=2 同时在线
uid=1 发消息
uid=2 页面收到消息
uid=2 ACK 后 IMServer 标记 Acked
```

### 第五步：离线补发

实现：

```text
to_uid 离线时保持 Pending
USER_ONLINE 时投递 Pending
```

验证：

```text
uid=2 离线
uid=1 发消息
uid=2 上线后收到离线消息
ACK 后状态变为 Acked
```

## 第一版不做

```text
群聊
多端登录
图片/文件消息
真实数据库用户系统
JWT 刷新
多 Gateway 路由
Gateway pending 持久化
Gateway 到 IMServer 连接池
历史消息查询
消息搜索
```

这些能力都可以在当前边界上继续演进，但不应该进入第一版。

## 成功标准

```text
1. 两个浏览器页面分别以 user_id=1、user_id=2 和默认密码登录。
2. uid=1 给 uid=2 发送文本消息。
3. uid=1 收到 send_ack。
4. uid=2 实时收到 message。
5. uid=2 自动发送 message_ack。
6. IMServer 将该消息标记为 Acked。
7. uid=2 离线时，uid=1 发送的消息进入 Pending。
8. uid=2 重新上线后收到离线消息，并 ACK 成功。
```
