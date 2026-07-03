#ifndef CODEC_H

#define CODEC_H

#include <arpa/inet.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Message.pb.h"
 
// MessageCodec 负责在 TCP 字节流和 Protobuf Envelope 之间转换。
//
// TCP 是流式协议，不保留消息边界，所以不能直接把 protobuf 裸发出去。
// 这里统一使用如下帧格式：
//
//   4 字节网络序 body 长度 + 序列化后的 message::Envelope
//
// 服务端和客户端都可以使用这个类。
// 这个类本身不保存接收缓存，只提供两个静态方法：
//   1. Encode()：把 Envelope 编码成完整 TCP 帧。
//   2. DecodeAll()：从传入的字节流中解析出完整 Envelope，并返回已消费字节数。
//
// 注意：半包缓存是连接级状态，应该由 ChatServer/ChatClient 为每条连接维护，
// 不应该放在这个无状态工具类里。
class MessageCodec {
public:
    enum class DecodeStatus {
        // 当前传入的数据已经全部解析完成，没有剩余字节。
        kOk,

        // 当前传入的数据末尾还有半包，需要等待下一次 TCP 数据到达。
        kNeedMoreData,

        // 长度头非法，例如长度为 0 或超过最大帧限制。
        kInvalidLength,

        // 长度合法，但 protobuf 反序列化失败。
        kParseError,
    };

    struct DecodeResult {
        DecodeStatus status = DecodeStatus::kNeedMoreData;

        // 本次成功解析出的完整 Envelope。一次 TCP 读取可能包含多条消息。
        std::vector<message::Envelope> envelopes;

        // 本次成功解析并消费的字节数。
        // 调用方负责根据该长度丢弃已处理数据，保留剩余半包。
        std::size_t consumed_bytes = 0;
    };

    static const std::size_t kHeaderLength = 4;
    static const std::size_t kDefaultMaxFrameLength = 1024 * 1024;//默认最大处理帧长度为 1MB

    // 把一个 Envelope 编码成可直接写入 TCP 的完整帧。
    // 返回空字符串表示 protobuf 序列化失败。
    static std::string Encode(const message::Envelope& envelope) {
        //序列化为字符串
        std::string body;
        if (!envelope.SerializeToString(&body)) {
            return std::string();
        }
        //计算消息体长度并转换为网络字节序
        uint32_t body_length = static_cast<uint32_t>(body.size());
        uint32_t network_length = htonl(body_length);
        
        //构造完整帧：4 字节长度 + 消息体
        std::string frame;
        frame.resize(kHeaderLength + body.size());
        std::memcpy(&frame[0], &network_length, kHeaderLength);
        std::memcpy(&frame[MessageCodec::kHeaderLength], body.data(), body.size());
        return frame;
    }

    // 从 data 中尽可能解析出完整 Envelope。
    //
    // 返回值中的 consumed_bytes 表示已经成功解析并消费的字节数。
    // 如果 status 是 kNeedMoreData，调用方应保留 consumed_bytes 之后的数据，
    // 等待下一次收到更多字节后继续 DecodeAll。
    // 如果 status 是协议错误，调用方通常应该关闭连接。
    static DecodeResult DecodeAll( const char* data,
                                std::size_t len,
                                std::size_t max_frame_length = kDefaultMaxFrameLength) 
    {
        //返回的结果结构体
        DecodeResult result;
        std::size_t offset = 0;
            
        while (offset < len) {
            std::size_t remaining_bytes = len - offset;
            //检查是否有足够的剩余字节读取长度头
            if (remaining_bytes < kHeaderLength) {
                result.status = DecodeStatus::kNeedMoreData;
                result.consumed_bytes = offset;
                return result;
            }
            //从剩余数据中提取长度头
            uint32_t network_length = 0;
            std::memcpy(&network_length, data + offset, kHeaderLength);
            uint32_t body_length = ntohl(network_length);

            //如果长度非法，返回错误状态和已消费字节数
            if (body_length == 0 || body_length > max_frame_length) {
                result.status = DecodeStatus::kInvalidLength;
                result.consumed_bytes = offset;
                return result;
            }
            //获取完整帧长度并检查剩余字节是否足够
            std::size_t frame_length = kHeaderLength + body_length;
            if (remaining_bytes < frame_length) {
                result.status = DecodeStatus::kNeedMoreData;
                result.consumed_bytes = offset;
                return result;
            }

            message::Envelope envelope;
            if (!envelope.ParseFromArray(data + offset + kHeaderLength,
                                         static_cast<int>(body_length))) {
                result.status = DecodeStatus::kParseError;
                result.consumed_bytes = offset;
                return result;
            }

            result.envelopes.push_back(envelope);
            offset += frame_length;
        }

        result.status = DecodeStatus::kOk;
        result.consumed_bytes = offset;
        return result;
    }

    static DecodeResult DecodeAll( const std::string& data,
                                std::size_t max_frame_length = kDefaultMaxFrameLength) 
    {
        return DecodeAll(data.data(), data.size(), max_frame_length);
    }
};

#endif
