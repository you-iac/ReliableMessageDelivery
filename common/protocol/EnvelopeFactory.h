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
    static message::Envelope CreateLoginRequest(uint64_t seq, uint64_t uid) {
        message::Envelope envelope;
        envelope.set_type(message::LOGIN_REQ);
        envelope.set_seq(seq);
        envelope.set_timestamp_ms(NowMs());

        message::LoginRequest* request = envelope.mutable_login_req();
        request->set_uid(uid);

        return envelope;
    }

    static message::Envelope CreateLoginResponse(uint64_t seq,
                                                 bool ok,
                                                 const std::string& reason) {
        message::Envelope envelope;
        envelope.set_type(message::LOGIN_RESP);
        envelope.set_seq(seq);
        envelope.set_timestamp_ms(NowMs());

        message::LoginResponse* response = envelope.mutable_login_resp();
        response->set_ok(ok);
        response->set_reason(reason);

        return envelope;
    }

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

    static message::Envelope CreateChatPush(uint64_t seq,
                                            uint64_t msg_id,
                                            uint64_t from_uid,
                                            uint64_t to_uid,
                                            const std::string& content,
                                            uint64_t server_timestamp_ms = 0,
                                            bool history = false) {
        message::Envelope envelope;
        envelope.set_type(message::CHAT_PUSH);
        envelope.set_seq(seq);
        envelope.set_timestamp_ms(NowMs());

        message::ChatPush* push = envelope.mutable_chat_push();
        push->set_msg_id(msg_id);
        push->set_from_uid(from_uid);
        push->set_to_uid(to_uid);
        push->set_content(content);
        push->set_server_timestamp_ms(
            server_timestamp_ms == 0 ? NowMs() : server_timestamp_ms);
        push->set_history(history);

        return envelope;
    }

    static message::Envelope CreateAck(uint64_t seq,
                                       uint64_t msg_id,
                                       bool ok,
                                       const std::string& reason) {
        message::Envelope envelope;
        envelope.set_type(message::ACK);
        envelope.set_seq(seq);
        envelope.set_timestamp_ms(NowMs());

        message::Ack* ack = envelope.mutable_ack();
        ack->set_msg_id(msg_id);
        ack->set_seq(seq);
        ack->set_ok(ok);
        ack->set_reason(reason);

        return envelope;
    }

private:
    //获取当前时间戳（毫秒）
    static uint64_t NowMs() {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()).count());
    }
};

#endif
