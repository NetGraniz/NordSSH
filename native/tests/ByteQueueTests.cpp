#include "ssh/ByteQueue.h"
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

using nordssh::ssh::ByteQueue;
static void require(bool result) { if (!result) throw std::runtime_error("Byte queue invariant failed"); }
int main() {
    try {
        ByteQueue queue;
        std::array<std::byte, ByteQueue::MaxChunk> data{};
        std::array<std::byte, ByteQueue::MaxChunk + 1> oversized{};
        require(!queue.tryPush({}));
        require(!queue.tryPush(oversized));
        for (std::size_t i = 0; i < ByteQueue::MaxBytes / data.size(); ++i) require(queue.tryPush(data));
        require(queue.usage().bytes == ByteQueue::MaxBytes);
        require(!queue.roomForChunk() && !queue.tryPush(std::span(data).first(1)));
        auto chunk = queue.tryPop();
        require(chunk && chunk->bytes().size() == data.size() && queue.roomForChunk());
        queue.close();
        require(queue.usage().closed && queue.usage().bytes == 0 && queue.usage().chunks == 0);
        require(!queue.tryPop() && !queue.tryPush(data) && !queue.roomForChunk());
        ByteQueue tiny;
        for (std::size_t i = 0; i < ByteQueue::MaxChunks; ++i) require(tiny.tryPush(std::span(data).first(1)));
        require(tiny.usage().bytes == ByteQueue::MaxChunks && !tiny.tryPush(std::span(data).first(1)));
        require(!tiny.roomForChunk());
        ByteQueue concurrent;
        std::atomic<bool> failed{false};
        std::jthread producer([&](std::stop_token stop) {
            for (unsigned i = 0; i < 10000 && !stop.stop_requested(); ++i) {
                const std::array<std::byte, 2> message{std::byte(i & 255), std::byte(i >> 8)};
                while (!concurrent.tryPush(message)) {
                    if (stop.stop_requested()) return;
                    std::this_thread::yield();
                }
            }
        });
        for (unsigned i = 0; i < 10000; ++i) {
            std::optional<nordssh::vault::SecretBytes> next;
            while (!(next = concurrent.tryPop())) std::this_thread::yield();
            if (next->bytes().size() != 2 || next->bytes()[0] != std::byte(i & 255) ||
                next->bytes()[1] != std::byte(i >> 8)) failed = true;
            const auto usage = concurrent.usage();
            require(usage.bytes <= ByteQueue::MaxBytes && usage.chunks <= ByteQueue::MaxChunks);
        }
        producer.join();
        require(!failed && concurrent.usage().bytes == 0);
        std::cout << "Byte/chunk bounds, atomic rejection, FIFO and concurrent exchange passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return EXIT_FAILURE; }
}
