#ifndef ENVELOPE_INSPECTOR_H
#define ENVELOPE_INSPECTOR_H

#include <sstream>
#include <string>

#include "Message.pb.h"

// EnvelopeInspector 负责检查和验证 message::Envelope 对象的内容是否合法。
class EnvelopeInspector {
    public:

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
                    << ", history=" << (envelope.chat_push().history() ? "true" : "false")
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
};
#endif 
