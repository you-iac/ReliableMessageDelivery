# 2026-06-11 Redis MessageStore 性能排查记录

## 背景

`MessageStore` 从内存版切换到 Redis 版后，压测吞吐明显下降。

内存版阶段可以达到约 `40000 msg/s` 级别；Redis 版在 `10 x 10000` 消息压测下，发送端写入很快，但服务端 ACK 吞吐下降到几千每秒。

最近一次观察：

```text
./client 10 10000 false 100

total_messages:           100000
send_elapsed_seconds:     0.152
ack_elapsed_seconds:      26.58
push_elapsed_seconds:     18.272
sender_acks:              100000
receiver_chat_pushes:     143665
client_write_throughput:  657895 msg/s
server_ack_throughput:    3762.23 msg/s
receiver_push_throughput: 7862.58 msg/s
```

客户端写入不是瓶颈，瓶颈主要出现在服务端业务处理和 Redis 同步访问路径。

## 已完成调整

### 1. `getMessage` 接口整理

`MessageStore::getMessage` 从指针 out 参数改为引用 out 参数：

```cpp
bool getMessage(uint64_t msg_id, MessageRecord& out);
```

这样调用方不用传裸指针，接口语义更清楚：返回值表示是否查询成功，`out` 只在返回 `true` 时可用。

### 2. `MessageStore.cpp` 补充必要注释

为 Redis key 构造、Lua 脚本、reply 解析、连接管理和状态更新函数补充了说明性注释。

重点说明了：

- 幂等 key、消息 hash、Pending zset、Delivered timeout zset 的用途。
- Lua 脚本负责的原子状态转换。
- hiredis `redisReply` 生命周期管理。
- `MessageRecord` 字段解析和 Redis reply 字段顺序约束。

### 3. 投递函数改为直接接收 `MessageRecord`

原来的投递接口：

```cpp
bool tryDeliverMessage(uint64_t msg_id, bool is_retry);
```

会在内部再次 `getMessage(msg_id)`，导致新消息创建后投递时多一次 Redis 读取。

现在拆成：

```cpp
bool tryDeliverMessage(const MessageRecord& record);
bool retryDeliverMessage(const MessageRecord& record);
```

调用方式：

```text
handleChat()            -> tryDeliverMessage(record)
deliverPendingMessages() -> tryDeliverMessage(record)
deliveryLoop()          -> retryDeliverMessage(record)
```

普通投递不再因为只有 `msg_id` 而额外查 Redis。重试投递单独递增 `retry_count`，语义比 `bool is_retry` 清楚。

### 4. ACK 路径合并 Redis 访问

原 ACK 流程：

```text
handleAck()
  -> getMessage(msg_id)        // 查 to_uid
  -> 校验 ack uid
  -> markAcked/markFailed
```

现在改为：

```text
handleAck()
  -> 获取当前连接 uid
  -> markAcked(msg_id, ack_uid)
```

`kMarkAckedScript` 在 Redis 内部完成：

```text
1. HGET msg_key to_uid
2. 如果消息不存在，返回失败
3. 如果 to_uid != ack_uid，返回失败
4. 更新 status = Acked
5. 写 acked_at_ms
6. 从 delivered_timeout 和 pending:<uid> 索引删除
```

这样 ACK 路径从两次 Redis 访问减少为一次 Redis Lua 调用。

同时移除了接收方 `ack.ok=false` 直接标记 Failed 的分支。`markFailed()` 仍保留给服务端重试超过上限时使用。

## 当前 Redis 访问模型

经过以上调整后，一条正常在线消息仍然至少需要三次同步 Redis 操作：

```text
1. createMessage      EVAL，创建 Pending 记录和幂等索引
2. markDelivered      EVAL，发送 push 后标记 Delivered
3. markAcked          EVAL，接收方 ACK 后标记 Acked
```

所以 `100000` 条消息至少对应约 `300000` 次同步 Redis 往返。

如果发生服务端重投，实际操作数会更多。最近一次压测中：

```text
receiver_chat_pushes: 143665
```

说明除了 100000 条正常 push，还发生了 43665 次左右的重复 push。重复 push 又会带来更多 ACK 和 Redis 更新压力。

## 为什么第二次压测会受影响

Redis 数据会跨进程保留。压测结束时，如果还有消息没有最终进入 `Acked`，它们可能留在：

```text
rmd:pending:1
rmd:delivered_timeout
```

第二次压测 receiver 仍然使用 `uid=1`，登录后服务端会执行：

```text
deliverPendingMessages(1)
```

这会把上一轮残留的 Pending 消息补发给本轮 receiver。

压测客户端当前的完成条件是：

```cpp
sender_acks >= total_messages &&
receiver_chat_pushes >= total_messages
```

`receiver_chat_pushes` 只是收到的 push 包数量，不区分本轮消息、旧消息或重复消息。因此旧 Pending 消息会污染第二轮压测结果，甚至让第二轮很快达到完成条件。

排查命令：

```bash
redis-cli ZCARD rmd:pending:1
redis-cli ZCARD rmd:delivered_timeout
redis-cli ZRANGE rmd:pending:1 0 9 WITHSCORES
redis-cli HGETALL rmd:msg:<msg_id>
```

干净压测前建议停掉 server 后清理本项目 key：

```bash
redis-cli --scan --pattern 'rmd:*' | xargs -r redis-cli DEL
```

如果 Redis DB 只给本项目使用，也可以：

```bash
redis-cli FLUSHDB
```

## 当前主要瓶颈判断

### 1. 单业务 worker

`ServerEventDispatcher` 当前只有一个业务 worker。所有 `CHAT_REQ`、接收方 ACK、登录、断开事件都会进入同一个队列串行处理。

这意味着即使客户端写入吞吐很高，服务端业务处理仍然被单线程消费速度限制。

### 2. 同步 Redis 热路径

`MessageStore` 使用同步 hiredis 调用，每次 Redis 操作都会阻塞当前业务线程等待回复。

同时 `MessageStore` 只有一个 Redis 连接，并通过 `redis_mutex_` 串行化所有 Redis 操作。当前单 worker 下锁竞争不是最大问题，但它意味着未来即使增加 worker，Redis 访问仍会在这把锁上排队。

### 3. 发送方 ACK 被投递路径拖住

当前 `handleChat()` 中，发送方 ACK 在下面流程之后才返回：

```text
createMessage
tryDeliverMessage
markDelivered
send sender ACK
```

也就是说，发送方 ACK 吞吐被在线投递和 `markDelivered` Redis 写入拖住。

如果语义允许，可以改为：

```text
createMessage 成功后立即 ACK sender
随后再 tryDeliverMessage(record)
```

这样 `server_ack_throughput` 更接近“服务端可靠接收/建档吞吐”，不会被接收方投递路径一起拖慢。

### 4. ACK 超时时间过短导致重投放大

当前 `kAckTimeoutMs = 3000`。

在 100000 条消息压测下，单 worker + 同步 Redis 处理 ACK 很容易超过 3 秒。此时后台 `deliveryLoop()` 会把尚未处理到 ACK 的 Delivered 消息当成超时消息重投，造成：

```text
receiver_chat_pushes > total_messages
更多接收方 ACK
更多 Redis 写入
业务队列进一步积压
```

压测时可以先把 ACK 超时调大到 `30s` 或 `60s`，先消除重投风暴对吞吐基线的干扰。

### 5. 超时扫描存在 N+1 查询风险

`getTimeoutDeliveredMessages()` 当前流程：

```text
ZRANGEBYSCORE delivered_timeout
for each msg_id:
    HMGET rmd:msg:<id>
```

如果超时集合很大，后台线程会在 `redis_mutex_` 下做大量 Redis 查询，前台业务处理会被阻塞。

后续应考虑：

- `ZRANGEBYSCORE ... LIMIT 0 N`
- 每轮只处理固定数量超时消息
- 用 Lua 或 pipeline 批量取记录

## 下一步建议

优先级从高到低：

1. **发送方 ACK 提前**
   - `createMessage()` 成功后立即给 sender ACK。
   - 投递和 `markDelivered()` 放在 ACK 之后执行。

2. **压测时调大 ACK 超时**
   - 先把 `kAckTimeoutMs` 调到 `30000` 或 `60000`。
   - 目的是先测 Redis 建档/ACK 基线，避免重投放大。

3. **给超时扫描加 LIMIT**
   - 避免后台扫描一次处理过多超时消息。
   - 降低后台线程长时间占用 Redis 连接的风险。

4. **压测客户端按唯一消息统计**
   - 现在 `receiver_chat_pushes` 是包数量，不是唯一消息数。
   - 应按 `msg_id` 去重后统计本轮唯一 push。

5. **再考虑多 worker / Redis 连接池 / pipeline**
   - 多 worker 需要先处理事件顺序和共享状态。
   - Redis 连接池可以解除单连接锁瓶颈。
   - pipeline 或 Lua 批量化适合 ACK 和批量查询场景。

## 当前结论

Redis 版吞吐低不是因为 Redis 单条命令慢，而是当前架构把高频消息链路做成了：

```text
单 worker 串行
+ 每条消息多次同步 Redis 往返
+ 单 Redis 连接
+ 过早超时重投
+ 压测统计被重复 push 和历史 pending 污染
```

短期最值得先做的是：

```text
createMessage 成功后提前 ACK sender
调大 kAckTimeoutMs
限制超时扫描批量
```

这样可以先把“可靠建档吞吐”和“投递/重投吞吐”拆开观察。