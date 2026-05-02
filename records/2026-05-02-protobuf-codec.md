# 开发记录

记录项目设计过程、关键实现、踩坑问题和后续计划。

## 2026-05-02 17:22 CST

### 本阶段目标

实现基于 Protobuf 的消息封装和 TCP 字节流传输，让客户端可以发送封装后的 `Envelope`，服务端可以接收、解码并返回数据。

### 已完成事项

1. 设计 `Message.proto`
   - 使用 `Envelope` 作为统一消息外壳。
   - 使用 `MessageType` 区分业务消息类型。
   - 当前包含 `ChatRequest`、`ChatPush`、`Ack`、`Heartbeat` 等结构。
   - `ChatRequest` 中保留 `client_msg_id`，用于后续实现客户端重试去重。
   - `ChatPush` 使用服务端生成的 `msg_id`，用于后续 ACK、重试、追踪。

2. 实现 `MessageCodec`
   - 负责 `Envelope` 和 TCP 字节流之间的转换。
   - 发送格式：

     ```text
     4 字节网络序 body 长度 + 序列化后的 Envelope
     ```

   - `Encode()` 负责把 `Envelope` 编码成可发送的二进制帧。
   - `DecodeAll()` 负责从字节流中解析出一个或多个完整 `Envelope`。
   - 解码结果通过 `DecodeResult` 返回：
     - `envelopes`：本次成功解析出的完整消息。
     - `consumed_bytes`：本次成功消费的字节数。
     - `status`：解码状态，例如成功、半包、长度非法、反序列化失败。

3. 实现 `EnvelopeFactory`
   - 负责创建常用的 `Envelope` 对象。
   - 当前支持创建 `ChatRequest`。
   - 自动填充 `type`、`seq`、`timestamp_ms` 和 `chat_req` 字段。
   - 提供 `ToString()`，用于把 `Envelope` 转成可读字符串，方便日志和调试。

4. 整理公共协议库
   - 将 `Message.proto` 生成代码、`MessageCodec`、`EnvelopeFactory` 作为公共协议层。
   - client 和 server 都链接同一个公共库，保证双方使用一致的协议定义。

5. 跑通基础收发
   - 客户端创建 `ChatRequest`。
   - 使用 `MessageCodec::Encode()` 编码为 TCP frame。
   - 服务端接收 frame，并使用 `MessageCodec::DecodeAll()` 解码。
   - 当前服务端行为类似 echo server，会把收到的原始 frame 返回给客户端。

### 设计记录

#### TCP 字节流拆包和组包

TCP 是流式协议，不保留消息边界，所以不能直接发送裸 Protobuf 数据。

可能出现的情况：

```text
一次 recv 收到半个包
一次 recv 收到一个完整包
一次 recv 收到多个完整包
一次 recv 收到多个完整包加半个包
```

因此需要在 Protobuf 数据前加长度头：

```text
[4 字节长度][Protobuf body]
```

`MessageCodec` 只负责告诉调用者已经成功解析并消费了多少字节，剩余半包由 client/server 自己维护。

#### `DecodeResult` 为什么返回 consumed_bytes

最初考虑过让 `DecodeResult` 返回 `remaining` 字符串，但这样会复制剩余数据。

现在改为返回：

```cpp
std::size_t consumed_bytes;
```

调用方自己执行：

```cpp
buffer.erase(0, result.consumed_bytes);
```

或者在 Muduo 中：

```cpp
buffer->retrieve(result.consumed_bytes);
```

这样更贴近网络库的使用方式，也减少不必要的数据拷贝。

### 踩坑记录

#### 1. 二进制 frame 不能用 strlen 计算长度

现象：

```text
TCP 连接成功
客户端打印 sent to server
但服务端 onMessage 没有触发
客户端卡在 recv
```

问题代码：

```cpp
send(sock, msgbuff, strlen(msgbuff), 0);
```

原因：

Protobuf frame 前 4 字节是网络序长度头，例如：

```text
00 00 00 2A
```

第一个字节可能就是 `\0`。`strlen()` 遇到 `\0` 会认为字符串结束，所以实际可能发送了 0 字节。

正确做法：

```cpp
send(sock, frame.data(), frame.size(), 0);
```

并且要处理 `send()` 只发送部分数据的情况。

#### 2. 收到二进制 frame 后不能直接用 cout 打印 char*

现象：

```text
Received from server:
```

看起来像没有收到内容。

原因：

服务端 echo 回来的仍然是二进制 frame，开头可能是 `\0`。如果用：

```cpp
std::cout << buf << std::endl;
```

`buf` 会被当成 C 字符串，遇到第一个 `\0` 就停止输出。

正确做法：

```cpp
std::string received(buf, recvd);
auto result = MessageCodec::DecodeAll(received);
```

然后打印解码后的 `Envelope`。

#### 3. Muduo 日志流不能使用 std::endl

问题代码：

```cpp
LOG_INFO << "Received raw data: " << msg.size() << " bytes" << std::endl;
```

原因：

`LOG_INFO` 使用的是 Muduo 自己的 `LogStream`，不是 `std::ostream`，不支持 `std::endl` 这种 I/O 操纵器。

正确写法：

```cpp
LOG_INFO << "Received raw data: " << msg.size() << " bytes";
```

### 后续计划

1. 将服务端从 echo 行为改为真正的业务处理：
   - 解码 `Envelope`。
   - 根据 `MessageType` 分发处理。
   - 对 `ChatRequest` 返回业务 ACK。

2. 新增服务端消息处理类：
   - `MessageCodec` 继续负责字节流编码解码。
   - `EnvelopeFactory` 继续负责创建消息。
   - 新类负责解释和处理每一个 `Envelope`，例如 `EnvelopeHandler` 或 `MessageDispatcher`。

3. 实现客户端接收回包后的解码逻辑：
   - 维护接收缓冲区。
   - 调用 `MessageCodec::DecodeAll()`。
   - 打印或处理服务端返回的 `Envelope`。

4. 后续再扩展可靠消息能力：
   - 服务端生成 `msg_id`。
   - 返回 `Ack`。
   - 接收方 `ConsumeAck`。
   - 离线消息存储。
   - 重试和去重。
