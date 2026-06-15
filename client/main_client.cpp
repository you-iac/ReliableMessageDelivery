#include "Client.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int ParsePositiveInt(const char* value, int fallback) {
    if (value == nullptr) {
        return fallback;
    }

    int parsed = std::atoi(value);
    return parsed > 0 ? parsed : fallback;
}

int CountByType(const std::vector<message::Envelope>& messages,
                message::MessageType type) {
    int count = 0;
    for (const auto& envelope : messages) {
        if (envelope.type() == type) {
            ++count;
        }
    }
    return count;
}

struct MessageCounts {
    int login_resps = 0;
    int acks = 0;
    int chat_pushes = 0;
};

int TotalSenderAcks(const std::vector<std::unique_ptr<Client>>& senders) {
    int total = 0;
    for (const auto& sender : senders) {
        total += sender->getAckCount();
    }
    return total;
}

int TotalSenderLoginResponses(
    const std::vector<std::unique_ptr<Client>>& senders) {
    int total = 0;
    for (const auto& sender : senders) {
        total += sender->getLoginResponseCount();
    }
    return total;
}

void StopSendersConcurrently(std::vector<std::unique_ptr<Client>>& senders) {
    const std::size_t kCloseGroupSize = 10;
    std::vector<std::thread> close_workers;
    close_workers.reserve((senders.size() + kCloseGroupSize - 1) /
                          kCloseGroupSize);

    for (std::size_t begin = 0; begin < senders.size();
         begin += kCloseGroupSize) {
        std::size_t end = begin + kCloseGroupSize;
        if (end > senders.size()) {
            end = senders.size();
        }

        close_workers.emplace_back([&senders, begin, end] {
            for (std::size_t i = begin; i < end; ++i) {
                senders[i]->stopClient();
            }
        });
    }

    for (auto& worker : close_workers) {
        worker.join();
    }
}

int RunStressTest(int sender_count,
                  int messages_per_sender,
                  bool verbose,
                  int wait_timeout_seconds) {
    const int total_messages = sender_count * messages_per_sender;

    std::cout << "stress config: senders=" << sender_count
              << ", messages_per_sender=" << messages_per_sender
              << ", total_messages=" << total_messages
              << ", verbose=" << (verbose ? "true" : "false")
              << ", wait_timeout_seconds=" << wait_timeout_seconds << "\n";

    Client receiver;
    receiver.setVerbose(verbose);
    const uint64_t receiver_uid = 1;
    if (!receiver.startClient(receiver_uid)) {
        receiver.stopClient();
        return 1;
    }

    std::vector<std::unique_ptr<Client>> senders;
    senders.reserve(static_cast<std::size_t>(sender_count));
    for (int i = 0; i < sender_count; ++i) {
        std::unique_ptr<Client> sender(new Client());
        sender->setVerbose(verbose);
        uint64_t sender_uid = static_cast<uint64_t>(1000 + i);
        if (!sender->startClient(sender_uid)) {
            std::cerr << "failed to start sender uid=" << sender_uid << "\n";
            sender->stopClient();
            StopSendersConcurrently(senders);
            receiver.stopClient();
            return 1;
        }
        senders.push_back(std::move(sender));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::atomic<int> send_ok{0};
    std::atomic<int> send_failed{0};
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(sender_count));

    auto started_at = std::chrono::steady_clock::now();
    for (int i = 0; i < sender_count; ++i) {
        workers.emplace_back([&, i] {
            uint64_t sender_uid = static_cast<uint64_t>(1000 + i);
            for (int n = 0; n < messages_per_sender; ++n) {
                std::ostringstream oss;
                oss << "stress message sender=" << sender_uid
                    << " index=" << n;

                if (senders[static_cast<std::size_t>(i)]->sendMessage(
                        receiver_uid, oss.str())) {
                    ++send_ok;
                } else {
                    ++send_failed;
                }
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    auto send_finished_at = std::chrono::steady_clock::now();
    double send_elapsed_seconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            send_finished_at - started_at).count() / 1000.0;

    int sender_acks = 0;
    int receiver_pushes = 0;
    auto wait_started_at = std::chrono::steady_clock::now();
    auto ack_completed_at = wait_started_at;
    auto push_completed_at = wait_started_at;
    bool ack_done = false;
    bool push_done = false;

    while (true) {
        sender_acks = TotalSenderAcks(senders);
        receiver_pushes = receiver.getChatPushCount();

        auto now = std::chrono::steady_clock::now();
        if (!ack_done && sender_acks >= total_messages) {
            ack_done = true;
            ack_completed_at = now;
        }
        if (!push_done && receiver_pushes >= total_messages) {
            push_done = true;
            push_completed_at = now;
        }

        // 即使本轮计数已经达标，也继续等到超时，方便 receiver 消费历史 Pending。
        int waited_seconds = static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(
                now - wait_started_at).count());
        if (waited_seconds >= wait_timeout_seconds) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto finished_at = std::chrono::steady_clock::now();
    MessageCounts receiver_counts;
    receiver_counts.login_resps = receiver.getLoginResponseCount();
    receiver_counts.acks = receiver.getAckCount();
    receiver_counts.chat_pushes = receiver.getChatPushCount();
    int sender_login_resps = TotalSenderLoginResponses(senders);

    double ack_elapsed_seconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            (ack_done ? ack_completed_at : finished_at) - started_at).count() /
        1000.0;
    double push_elapsed_seconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            (push_done ? push_completed_at : finished_at) - started_at).count() /
        1000.0;
    double total_elapsed_seconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            finished_at - started_at).count() / 1000.0;

    std::cout << "\nStress summary\n";
    std::cout << "  send_elapsed_seconds:  " << send_elapsed_seconds << "\n";
    std::cout << "  ack_elapsed_seconds:   " << ack_elapsed_seconds << "\n";
    std::cout << "  push_elapsed_seconds:  " << push_elapsed_seconds << "\n";
    std::cout << "  total_elapsed_seconds: " << total_elapsed_seconds << "\n";
    std::cout << "  send_ok:               " << send_ok.load() << "\n";
    std::cout << "  send_failed:           " << send_failed.load() << "\n";
    std::cout << "  receiver_login_resps:  "
              << receiver_counts.login_resps << "\n";
    std::cout << "  receiver_chat_pushes:  "
              << receiver_counts.chat_pushes << "\n";
    std::cout << "  sender_login_resps:    " << sender_login_resps << "\n";
    std::cout << "  sender_acks:           " << sender_acks << "\n";
    if (send_elapsed_seconds > 0) {
        std::cout << "  client_write_throughput: "
                  << send_ok.load() / send_elapsed_seconds << " msg/s\n";
    }
    if (ack_elapsed_seconds > 0) {
        std::cout << "  server_ack_throughput:   "
                  << sender_acks / ack_elapsed_seconds << " msg/s\n";
    }
    if (push_elapsed_seconds > 0) {
        std::cout << "  receiver_push_throughput: "
                  << receiver_counts.chat_pushes / push_elapsed_seconds
                  << " msg/s\n";
    }

    StopSendersConcurrently(senders);
    receiver.stopClient();

    return (send_failed.load() == 0 &&
            sender_acks >= total_messages &&
            receiver_counts.chat_pushes >= total_messages)
               ? 0
               : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    const int sender_count = argc > 1 ? ParsePositiveInt(argv[1], 8) : 8;
    const int messages_per_sender =
        argc > 2 ? ParsePositiveInt(argv[2], 100) : 100;
    const bool verbose = argc > 3 && std::string(argv[3]) == "verbose";
    const int wait_timeout_seconds =
        argc > 4 ? ParsePositiveInt(argv[4], 30) : 30;
    
    return RunStressTest(sender_count,
                         messages_per_sender,
                         verbose,
                         wait_timeout_seconds);
    // Client c;
    // c.startClient(1);
    // sleep(1000 * 60);
    // c.stopClient();
}
