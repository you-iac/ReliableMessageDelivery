# ReliableMessageDelivery

ReliableMessageDelivery 是一个 C++11 实现的可靠消息投递服务原型，重点不是完整聊天业务，而是即时通信场景中的消息可靠性：

```text
在线实时投递
离线消息暂存
用户上线补发
发送方 ACK
接收方 ACK
超时重试
幂等去重
消息状态追踪
```

当前版本使用内存存储实现可靠投递闭环，适合学习 C++ 网络编程、Muduo Reactor、Protobuf 协议设计和消息可靠投递机制。

## 快速开始

构建项目：

```bash
cmake -S . -B build
cmake --build build
```

启动服务端：

```bash
./bin/server
```

服务端默认监听：

```text
0.0.0.0:8080
```

运行一次压测：

```bash
./bin/client 16 1000 quiet 60
```

当前压测模型是多个发送方客户端向一个接收方客户端发送消息，适合快速验证登录、投递、ACK、重试和消息状态流转是否闭环。

## 当前能力

已实现：

- 基于 Muduo 的 TCP 长连接服务。
- Protobuf `Envelope` 协议封装。
- `4 字节网络序长度头 + Protobuf body` 的 TCP 帧格式。
- 客户端登录绑定 uid。
- 服务端维护在线用户状态：
  - `uid -> TcpConnectionPtr`
  - `conn -> uid`
- 在线消息实时投递。
- 离线消息保存为 `Pending`。
- 用户重新登录后补发 `Pending` 消息。
- 发送方 ACK：服务端收到并处理 `CHAT_REQ` 后返回 ACK。
- 接收方 ACK：客户端收到 `CHAT_PUSH` 后返回 ACK。
- 服务端收到接收方 ACK 后将消息标记为 `Acked`。
- 消息状态机：
  - `Pending`
  - `Delivered`
  - `Acked`
  - `Failed`
- 客户端发送侧 `pending_acks` 和超时重试。
- 服务端 `Delivered` 超时扫描和重投。
- 基于 `from_uid + client_msg_id` 的服务端幂等去重。
- 多客户端压测入口。
- 开发记录文档。

当前未实现或未完善：

- 客户端按 `msg_id` 去重。
- `Acked / Failed` 消息清理。
- 严格按创建时间顺序补发离线消息。
- 心跳超时扫描和踢下线。
- MySQL 持久化。
- Redis 在线状态或未 ACK 缓存。
- `/metrics` 指标接口。
- 多 worker shard 并行业务处理。
- 生产级压测和限流策略。

## 架构概览

```text
client
  Client
    connectServer()
    sendMessage()
    receiverLoop()
    retryLoop()
    pending_acks

server
  ChatServer
    TCP 连接管理
    半包缓存
    Envelope 解码
    事件入队

  ServerEventDispatcher
    业务事件队列
    登录处理
    聊天处理
    ACK 处理
    离线补发
    超时重投

  UserStateService
    在线用户状态
    conn -> uid
    uid -> conn

  MessageStore
    内存消息账本
    状态流转
    幂等去重索引
```

## 协议模型

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

网络传输格式：

```text
4 字节大端 body 长度 + 序列化后的 message::Envelope
```

## 可靠投递流程

### 在线投递

```text
发送方 Client
  -> CHAT_REQ(from_uid, to_uid, content, client_msg_id)

服务端
  -> 校验连接已登录
  -> 校验 from_uid 和连接 uid 一致
  -> MessageStore::createMessage()
  -> 根据 from_uid + client_msg_id 幂等去重
  -> 目标在线则发送 CHAT_PUSH
  -> markDelivered(msg_id)
  -> 给发送方返回 ACK(msg_id)

接收方 Client
  -> 收到 CHAT_PUSH
  -> 存入业务消息队列
  -> 返回 ACK(msg_id)

服务端
  -> 校验 ACK 来自消息接收方
  -> markAcked(msg_id)
```

### 离线补发

```text
发送方发消息
  -> 目标用户不在线
  -> MessageStore 保存为 Pending
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

如果未超过最大重试次数：

```text
tryDeliverMessage(msg_id, true)
```

如果超过最大重试次数：

```text
markFailed(msg_id)
```

客户端也维护 `pending_acks`，对发送方 ACK 超时的 `CHAT_REQ` 进行重发。

## 幂等去重

客户端每条业务消息会生成 `client_msg_id`：

```text
uid + timestamp_ms + local_counter
```

服务端维护内存索引：

```text
from_uid + client_msg_id -> msg_id
```

如果客户端因为 ACK 超时重发同一个 `CHAT_REQ`，服务端会命中已有记录：

```text
不重复创建 MessageRecord
不重复投递 CHAT_PUSH
直接返回原 msg_id 的 ACK
```

注意：当前服务端已实现发送侧幂等去重，但客户端还未实现 `msg_id` 去重。服务端重投和接收方 ACK 并发时，客户端仍可能收到重复 `CHAT_PUSH`，后续需要在客户端按 `msg_id` 去重。

## 目录结构

```text
.
├── CMakeLists.txt
├── protocol
│   ├── Message.proto
│   ├── Codec.h
│   ├── EnvelopeFactory.h
│   └── EnvelopeInspector.h
├── client
│   ├── CMakeLists.txt
│   ├── include/Client.h
│   ├── src/Client.cpp
│   └── main_client.cpp
├── server
│   ├── CMakeLists.txt
│   ├── include
│   │   ├── ChatServer.h
│   │   ├── MessageStore.h
│   │   ├── ServerEventDispatcher.h
│   │   └── UserStateService.h
│   ├── src
│   │   ├── ChatServer.cpp
│   │   ├── MessageStore.cpp
│   │   ├── ServerEventDispatcher.cpp
│   │   └── UserStateService.cpp
│   └── main_service.cpp
├── records
└── record.md
```

## 环境依赖

当前项目依赖：

- C++11 编译器
- CMake
- Protobuf
- Muduo
- pthread

在 Ubuntu 环境中需要确保已安装 Protobuf 和 Muduo 开发库。Muduo 安装方式取决于你的本地环境，项目 CMake 目前直接链接：

```text
muduo_net
muduo_base
protobuf
pthread
```

## 构建

```bash
cmake -S . -B build
cmake --build build
```

构建产物默认输出到：

```text
bin/server
bin/client
```

## 运行

启动服务端：

```bash
./bin/server
```

运行客户端压测：

```bash
./bin/client [sender_count] [messages_per_sender] [verbose] [wait_timeout_seconds]
```

示例：

```bash
./bin/client 16 1000 quiet 60
```

参数说明：

```text
sender_count          发送方客户端数量
messages_per_sender   每个发送方发送的消息数
verbose               传 verbose 时打印逐条收发日志；其他值表示关闭详细日志
wait_timeout_seconds  等待 ACK/PUSH 完成的最大秒数
```

当前压测模型是：

```text
多个 sender -> 一个 receiver(uid=1)
```

因此所有 `CHAT_PUSH` 会集中到单个接收方连接。

## 压测输出说明

示例输出：

```text
Stress summary
  send_elapsed_seconds
  ack_elapsed_seconds
  push_elapsed_seconds
  total_elapsed_seconds
  send_ok
  send_failed
  receiver_login_resps
  receiver_chat_pushes
  sender_login_resps
  sender_acks
  client_write_throughput
  server_ack_throughput
  receiver_push_throughput
```

关键字段：

```text
send_ok
  客户端写入 socket 成功的消息数。

sender_acks
  发送方收到服务端 ACK 的数量。

receiver_chat_pushes
  接收方收到 CHAT_PUSH 的数量。
```

理想情况下：

```text
sender_acks == total_messages
receiver_chat_pushes == total_messages
```

但当前客户端还没有按 `msg_id` 去重，服务端重投可能导致 `receiver_chat_pushes` 大于 `total_messages`。因此现阶段该值代表收到的 PUSH 包数量，不等价于唯一业务消息数量。

## 当前性能观察

当前在虚拟机中，单 worker 业务模型下，小规模压测可以完整闭环：

```text
./bin/client 16 200 quiet 60

total_messages:        3200
sender_acks:           3200
receiver_chat_pushes:  3200
```

中高压力下会出现重复 PUSH：

```text
./bin/client 10 10000 quiet 60

total_messages:        100000
sender_acks:           100701
receiver_chat_pushes:  129052
```

主要原因：

```text
接收端尚未按 msg_id 去重
固定超时重投在队列积压时容易产生重复投递
服务端当前是单业务 worker
压测模型集中到单接收方连接
```

所以当前项目更适合验证可靠投递链路，而不是作为最终性能数据。

## 开发记录

开发过程记录见：

```text
record.md
records/
```

当前重要记录：

- `records/2026-05-02-protobuf-codec.md`
- `records/2026-05-03-online-forwarding.md`
- `records/2026-05-04-server-event-dispatcher-message-store-benchmark.md`
- `records/2026-05-05-reliable-delivery-ack-retry-idempotency.md`

## 后续计划

### 生产化差距

如果目标是做真实的消息后端，当前项目还处在单机内存原型阶段。后续需要优先补齐以下能力：

1. 持久化消息和投递状态。
   - 当前 `MessageStore` 使用内存 `unordered_map`，服务重启后消息、状态和幂等索引都会丢失。
   - 发送方 ACK 应调整为“消息和幂等记录持久化成功后再返回”。
2. 明确并收紧 ACK 语义。
   - 发送方 ACK 表示服务端已可靠接收。
   - 接收方 ACK 应表示客户端已经完成幂等处理，而不只是收到网络包。
3. 客户端按 `msg_id` 幂等消费。
   - 服务端重投和接收方 ACK 存在正常竞态，生产环境应默认按至少一次投递设计。
   - 重复 `CHAT_PUSH` 应只回 ACK，不重复进入业务消息队列。
4. 补齐故障恢复能力。
   - 服务重启后应能恢复 Pending、Delivered 未 ACK、重试次数和幂等索引。
   - 后续再考虑多实例、主备或分片部署。
5. 定义消息顺序模型。
   - 至少需要明确同一发送方到同一接收方、同一会话或同一用户收件箱内的顺序保证。
   - 离线补发应按 `created_at_ms` 或 `msg_id` 稳定排序。
6. 提升并发和背压能力。
   - 当前业务处理仍是单 worker，后续可按连接、uid 或会话做 shard worker。
   - 需要 inflight 限流、慢客户端处理和重试退避，避免积压时触发重投风暴。
7. 增加生产级保护。
   - 认证、TLS、心跳超时踢下线、消息大小限制、指标、告警、日志采样、容量清理和长期压测。

阶段目标应先聚焦“单机可靠”：持久化、恢复、幂等、顺序和可观测性稳定后，再推进多实例扩展。

### 功能计划

优先级较高：

1. 客户端按 `msg_id` 去重。
2. `Acked / Failed` 消息清理，避免内存持续增长。
3. 离线补发按 `created_at_ms` 排序。
4. 调整重试策略，避免压测时重投风暴。
5. 增加 inflight 限流。
6. 降级或关闭服务端逐条日志。

中长期：

1. 接入 MySQL 持久化消息。
2. 使用 Redis 维护在线状态或未 ACK 缓存。
3. 增加 `/metrics` 指标接口。
4. 心跳超时扫描。
5. 多 worker shard 队列提升多核利用率。

## 项目定位

当前项目是一个内存版可靠消息投递服务原型，重点展示：

```text
网络编程
Protobuf 协议
并发队列
ACK 确认
幂等去重
离线补偿
超时重试
状态机设计
```

它不是完整生产级 IM 系统。生产化还需要持久化、指标、限流、清理策略、更多测试和多实例扩展。
