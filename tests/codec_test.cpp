#include <arpa/inet.h>

#include <cstdint>
#include <iostream>
#include <string>

#include "Codec.h"

namespace {

bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        return false;
    }
    return true;
}

message::Envelope MakeEnvelope(uint64_t seq, const std::string& content) {
    message::Envelope envelope;
    envelope.set_type(message::CHAT_REQ);
    envelope.set_seq(seq);
    envelope.set_timestamp_ms(123456);
    message::ChatRequest* request = envelope.mutable_chat_req();
    request->set_from_uid(1);
    request->set_to_uid(2);
    request->set_content(content);
    request->set_client_msg_id("client-message-1");
    return envelope;
}

bool TestSingleFrame() {
    const std::string frame = MessageCodec::Encode(MakeEnvelope(7, "hello"));
    MessageCodec::DecodeResult result = MessageCodec::DecodeAll(frame);

    return Expect(!frame.empty(), "encoded frame should not be empty") &&
           Expect(result.status == MessageCodec::DecodeStatus::kOk,
                  "single frame should decode completely") &&
           Expect(result.consumed_bytes == frame.size(),
                  "single frame should be fully consumed") &&
           Expect(result.envelopes.size() == 1,
                  "single frame should contain one envelope") &&
           Expect(result.envelopes[0].seq() == 7,
                  "decoded sequence should match") &&
           Expect(result.envelopes[0].chat_req().content() == "hello",
                  "decoded content should match");
}

bool TestPartialFrame() {
    const std::string frame = MessageCodec::Encode(MakeEnvelope(8, "partial"));
    MessageCodec::DecodeResult result =
        MessageCodec::DecodeAll(frame.data(), frame.size() - 1);

    return Expect(result.status == MessageCodec::DecodeStatus::kNeedMoreData,
                  "partial frame should request more data") &&
           Expect(result.consumed_bytes == 0,
                  "partial frame should not be consumed") &&
           Expect(result.envelopes.empty(),
                  "partial frame should not emit an envelope");
}

bool TestMultipleFrames() {
    const std::string first = MessageCodec::Encode(MakeEnvelope(9, "first"));
    const std::string second = MessageCodec::Encode(MakeEnvelope(10, "second"));
    const std::string data = first + second;
    MessageCodec::DecodeResult result = MessageCodec::DecodeAll(data);

    return Expect(result.status == MessageCodec::DecodeStatus::kOk,
                  "combined frames should decode completely") &&
           Expect(result.consumed_bytes == data.size(),
                  "combined frames should be fully consumed") &&
           Expect(result.envelopes.size() == 2,
                  "combined frames should emit two envelopes") &&
           Expect(result.envelopes[0].seq() == 9 &&
                      result.envelopes[1].seq() == 10,
                  "combined frames should preserve order");
}

bool TestCompleteAndPartialFrames() {
    const std::string first = MessageCodec::Encode(MakeEnvelope(11, "complete"));
    const std::string second = MessageCodec::Encode(MakeEnvelope(12, "partial"));
    const std::string data = first + second.substr(0, second.size() - 1);
    MessageCodec::DecodeResult result = MessageCodec::DecodeAll(data);

    return Expect(result.status == MessageCodec::DecodeStatus::kNeedMoreData,
                  "trailing partial frame should request more data") &&
           Expect(result.consumed_bytes == first.size(),
                  "only the complete frame should be consumed") &&
           Expect(result.envelopes.size() == 1 &&
                      result.envelopes[0].seq() == 11,
                  "the complete frame should still be emitted");
}

bool TestInvalidLength() {
    uint32_t network_length = htonl(0);
    const std::string data(reinterpret_cast<const char*>(&network_length),
                           sizeof(network_length));
    MessageCodec::DecodeResult result = MessageCodec::DecodeAll(data);

    return Expect(result.status == MessageCodec::DecodeStatus::kInvalidLength,
                  "zero length should be rejected") &&
           Expect(result.consumed_bytes == 0,
                  "invalid frame should not be consumed");
}

}  // namespace

int main() {
    if (!TestSingleFrame() ||
        !TestPartialFrame() ||
        !TestMultipleFrames() ||
        !TestCompleteAndPartialFrames() ||
        !TestInvalidLength()) {
        return 1;
    }

    std::cout << "codec tests passed" << std::endl;
    return 0;
}
