# 开发记录

记录项目设计过程、关键实现、踩坑问题和后续计划。

## 2026-05-03 CST

### 本阶段目标

将项目从“Protobuf 编解码 + echo server”推进到第一版真正的在线消息转发：

```text
客户端登录绑定 uid
        ↓
发送方发送 CHAT_REQ
        ↓
服务端查找接收方是否在线
        ↓
在线：推送 CHAT_PUSH
        ↓
发送方收到 ACK
```

第一版只做在线实时转发，暂不实现离线存储、重试队列、去重表和消息状态持久化。

### 已完成事项

1. 补齐协议对象创建能力
   - `EnvelopeFactory` 新增 `CreateLoginRequest()`。
   - 新增 `CreateLoginResponse()`，用于服务端返回登录结果。
   - 新增 `CreateChatPush()`，用于服务端向接收方推送消息。
   - 新增 `CreateAck()`，用于服务端确认发送请求处理结果。

2. 拆分客户端结构
   - 新增 `Client` 类，封装一条用户侧 TCP 长连接。
   - `main_client.cpp` 只负责创建客户端、启动、发送消息和读取消息。
   - `Client` 内部维护：
     - `uid`
     - `sock`
     - 后台接收线程
     - 接收消息队列
     - 停止标记和已接收消息计数
   - `getMessage()` 使用互斥锁返回当前消息队列快照，避免和接收线程产生数据竞争。

3. 拆分服务端结构
   - 新增 `ChatServer` 类，封装 Muduo 服务器启动和业务处理。
   - `main_service.cpp` 只保留：

     ```cpp
     ChatServer server;
     server.start();
     ```

   - 服务端内部维护：
     - `ClientSession`：单连接状态，保存 `uid`、连接指针和半包缓存。
     - `sessions_`：连接指针到 session 的映射。
     - `online_users_`：`uid -> TcpConnectionPtr` 在线用户表。
     - `next_msg_id_`：第一版进程内自增消息 ID。

4. 实现服务端消息分发
   - `LOGIN_REQ`：
     - 校验 uid。
     - 将当前连接绑定到 uid。
     - 写入在线用户表。
     - 返回 `LOGIN_RESP`。
   - `CHAT_REQ`：
     - 校验当前连接是否已登录。
     - 校验 `from_uid` 是否和连接绑定 uid 一致。
     - 查找 `to_uid` 是否在线。
     - 在线时向接收方发送 `CHAT_PUSH`。
     - 向发送方返回 `ACK`。
   - 不支持的消息类型返回失败 ACK。

5. CMake 结构调整
   - client 改为：

     ```text
     client/main_client.cpp
     client/include/Client.h
     client/src/Client.cpp
     ```

   - server 改为：

     ```text
     server/main_service.cpp
     server/include/ChatServer.h
     server/src/ChatServer.cpp
     ```

   - `client` 和 `server` 都继续链接公共协议库 `message_protocol`。

### 设计记录

#### 为什么不叫 Mailbox

最初考虑过引入“邮箱类”负责收发消息，但这个名字容易把三个职责混在一起：

```text
网络连接
消息缓存
业务投递
```

当前阶段更适合使用：

```text
Client       客户端长连接封装
ChatServer   服务端启动和业务转发封装
ClientSession 单连接状态
online_users_ 在线用户索引
```

`Mailbox` 更适合后续表示离线消息收件箱，例如 `OfflineMailbox`。

#### 为什么 main 只调用 start

`main` 的职责应该是启动进程，而不是理解 Muduo 的回调注册、连接表、在线表和消息分发。

因此服务端入口收敛为：

```cpp
int main() {
    ChatServer server;
    if (!server.start()) {
        return 1;
    }
    return 0;
}
```

Muduo 的 `EventLoop`、`TcpServer`、回调注册和消息处理全部放入 `ChatServer` 内部。

#### 为什么接收队列需要锁

客户端接收线程会写入：

```cpp
message_queue.push_back(envelope);
```

主线程会读取：

```cpp
client.getMessage();
```

即使 `getMessage()` 只是读，只要另一个线程可能同时写同一个 `std::vector`，就会产生数据竞争。因此 `pushMessage()` 和 `getMessage()` 都要使用同一个互斥锁保护。

#### 为什么服务端解码后再释放锁分发

`onMessage()` 中先在锁内完成：

```cpp
session->recv_buffer.append(...)
DecodeAll(...)
envelopes.swap(decode_result.envelopes)
```

然后释放锁，再遍历 `envelopes` 调用 `dispatch()`。

这样可以避免业务处理期间长时间占用连接表锁，减少其他连接登录、断开和收消息时的阻塞。

### 踩坑记录

#### 1. std::string::append 重载误用

问题代码：

```cpp
recv_buffer.append(buf, static_cast<std::size_t>(recvd));
```

本来想表达：

```text
追加 buf 前 recvd 个字节
```

但因为 `buf` 是 `std::string`，实际匹配的是：

```cpp
append(const std::string& str, size_type subpos)
```

也就是从 `buf[recvd]` 开始追加到末尾。由于 `buf` 预分配了大量 `\0`，导致解码时读到非法长度。

正确写法：

```cpp
recv_buffer.append(buf.data(), static_cast<std::size_t>(recvd));
```

#### 2. 头文件 include 写错

客户端曾经写成：

```cpp
#include "MessageCodec.h"
```

但项目中实际协议编解码头文件是：

```cpp
#include "Codec.h"
```

最终 `Client.h` 不直接依赖编解码实现，只保留：

```cpp
#include "Message.pb.h"
```

`Client.cpp` 再 include：

```cpp
#include "Codec.h"
#include "EnvelopeFactory.h"
#include "EnvelopeInspector.h"
```

这样头文件依赖更少。

#### 3. Muduo Timestamp 命名空间

曾经误写：

```cpp
#include <muduo/net/Timestamp.h>
using Timestamp = muduo::net::Timestamp;
```

实际 Muduo 的 `Timestamp` 在 base 模块：

```cpp
#include <muduo/base/Timestamp.h>
using Timestamp = muduo::Timestamp;
```

#### 4. 头文件中不要写 using namespace std

头文件会被其他 `.cpp` include，如果在头文件里写：

```cpp
using namespace std;
```

会污染所有包含该头文件的翻译单元，容易引入命名冲突。

当前采用类内部类型别名减少长命名空间带来的噪音：

```cpp
using TcpConnectionPtr = muduo::net::TcpConnectionPtr;
using Buffer = muduo::net::Buffer;
using Timestamp = muduo::Timestamp;
using Envelope = message::Envelope;
```

### 运行结果测试

在线转发流程已经跑通，典型输出如下：

```text
send: Envelope{type=LOGIN_REQ, seq=1, login_req={uid=1}}
uid 1 received: Envelope{type=LOGIN_RESP, seq=1, login_resp={ok=true, reason=""}}

send: Envelope{type=LOGIN_REQ, seq=1, login_req={uid=2}}
uid 2 received: Envelope{type=LOGIN_RESP, seq=1, login_resp={ok=true, reason=""}}

send: Envelope{type=CHAT_REQ, seq=2, chat_req={from_uid=1, to_uid=2, content="Hello from user1!", client_msg_id="1-2"}}

uid 1 received: Envelope{type=ACK, seq=2, ack={msg_id=1, seq=2, ok=true, reason=""}}
uid 2 received: Envelope{type=CHAT_PUSH, seq=2, chat_push={msg_id=1, from_uid=1, to_uid=2, content="Hello from user1!"}}
```

说明：

```text
uid=1 登录成功
uid=2 登录成功
uid=1 发送 CHAT_REQ
服务端给 uid=2 推送 CHAT_PUSH
服务端给 uid=1 返回 ACK
```

### 后续计划

1. 完善客户端接口
   - `getMessage()` 当前返回快照，不清空队列。
   - 后续可以新增 `popMessages()`，表示取走当前所有消息。
   - `sendMessage()` 后续可以返回更明确的发送结果。

2. 增加服务端失败场景测试
   - 未登录发送 `CHAT_REQ`。
   - `from_uid` 和当前连接 uid 不一致。
   - 目标用户不在线。
   - 非法 frame 或 protobuf 解析失败。

3. 推进可靠投递能力
   - 服务端持久化消息。
   - 离线消息存储和重连补发。
   - 接收方消费 ACK。
   - 未 ACK 消息重试。
   - 基于 `client_msg_id` 和 `msg_id` 做幂等去重。

4. 整理命名和头文件依赖
   - 继续减少头文件中不必要的系统头。
   - 必要时考虑 Pimpl，让 `ChatServer.h` 更干净。
