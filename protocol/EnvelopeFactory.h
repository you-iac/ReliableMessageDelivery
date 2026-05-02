#ifndef ENVELOPE_FACTORY_H
#define ENVELOPE_FACTORY_H

#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>

#include "Message.pb.h"

// EnvelopeFactory 负责创建常用的 message::Envelope 对象。
//
// 它只关心“业务消息对象怎么填充”，不关心 TCP 编码、拆包和 protobuf 字节流；
// 字节流转换仍然由 MessageCodec 负责。
class EnvelopeFactory {
public:
    // 创建一条聊天发送请求。
    //
    // 生成的 Envelope 会自动填充：
    //   type         = CHAT_REQ
    //   seq          = 调用方传入的请求序号
    //   timestamp_ms = 当前毫秒时间戳
    //   chat_req     = 具体聊天请求内容
    static message::Envelope CreateChatRequest(
        uint64_t seq,
        uint64_t from_uid,
        uint64_t to_uid,
        const std::string& content,
        const std::string& client_msg_id) {
        
        //设置消息类型、序列号和时间戳
        message::Envelope envelope;
        envelope.set_type(message::CHAT_REQ);
        envelope.set_seq(seq);
        envelope.set_timestamp_ms(NowMs());
        //设置消息体内容
        message::ChatRequest* request = envelope.mutable_chat_req();
        request->set_from_uid(from_uid);
        request->set_to_uid(to_uid);
        request->set_content(content);
        request->set_client_msg_id(client_msg_id);

        return envelope;
    }
    // 将 Envelope 转成便于日志输出和调试阅读的字符串。
    // 这里不直接打印，调用方可以自行选择 LOG_INFO、std::cout 或测试断言。
    static std::string ToString(const message::Envelope& envelope)
    {
        std::ostringstream oss;
        oss << "Envelope{"
            << "type=" << MessageTypeToString(envelope.type())
            << ", seq=" << envelope.seq()
            << ", timestamp_ms=" << envelope.timestamp_ms();

        switch (envelope.payload_case()) {
            case message::Envelope::kLoginReq:
                oss << ", login_req={uid="
                    << envelope.login_req().uid()
                    << "}";
                break;
            case message::Envelope::kLoginResp:
                oss << ", login_resp={ok="
                    << (envelope.login_resp().ok() ? "true" : "false")
                    << ", reason=\"" << envelope.login_resp().reason()
                    << "\"}";
                break;
            case message::Envelope::kChatReq:
                oss << ", chat_req={from_uid="
                    << envelope.chat_req().from_uid()
                    << ", to_uid=" << envelope.chat_req().to_uid()
                    << ", content=\"" << envelope.chat_req().content()
                    << "\", client_msg_id=\"" << envelope.chat_req().client_msg_id()
                    << "\"}";
                break;
            case message::Envelope::kChatPush:
                oss << ", chat_push={msg_id="
                    << envelope.chat_push().msg_id()
                    << ", from_uid=" << envelope.chat_push().from_uid()
                    << ", to_uid=" << envelope.chat_push().to_uid()
                    << ", content=\"" << envelope.chat_push().content()
                    << "\", server_timestamp_ms=" << envelope.chat_push().server_timestamp_ms()
                    << "}";
                break;
            case message::Envelope::kAck:
                oss << ", ack={msg_id="
                    << envelope.ack().msg_id()
                    << ", seq=" << envelope.ack().seq()
                    << ", ok=" << (envelope.ack().ok() ? "true" : "false")
                    << ", reason=\"" << envelope.ack().reason()
                    << "\"}";
                break;
            case message::Envelope::kHeartbeat:
                oss << ", heartbeat={uid="
                    << envelope.heartbeat().uid()
                    << "}";
                break;
            case message::Envelope::PAYLOAD_NOT_SET:
                oss << ", payload=not_set";
                break;
        }

        oss << "}";
        return oss.str();
    }
private:
    
    static const char* MessageTypeToString(message::MessageType type) {
        switch (type) {
            case message::UNKNOWN:
                return "UNKNOWN";
            case message::LOGIN_REQ:
                return "LOGIN_REQ";
            case message::LOGIN_RESP:
                return "LOGIN_RESP";
            case message::CHAT_REQ:
                return "CHAT_REQ";
            case message::CHAT_PUSH:
                return "CHAT_PUSH";
            case message::ACK:
                return "ACK";
            case message::HEARTBEAT:
                return "HEARTBEAT";
            default:
                return "UNRECOGNIZED";
        }
    }

    //获取当前时间戳（毫秒）
    static uint64_t NowMs() {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()).count());
    }
};

#endif