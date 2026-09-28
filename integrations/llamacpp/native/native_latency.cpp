// NIC completion timing with persistent MRs over actual GPU tensor allocations.
#include "ggml.h"
#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "mcdma-rdma.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using timer = std::chrono::steady_clock;
static void check(bool value, const char * reason) { if (!value) throw std::runtime_error(reason); }
static double elapsed_us(timer::time_point start) {
    return std::chrono::duration<double, std::micro>(timer::now() - start).count();
}
struct config {
    std::string mode, device, address, output;
    int port = 0, warmup = 1000, iterations = 10000;
    bool client() const { return mode == "client"; }
};
static int number(const std::string & text, int low, int high) {
    size_t used = 0;
    long value = std::stol(text, &used);
    check(used == text.size() && value >= low && value <= high, "invalid integer option");
    return (int) value;
}
static config arguments(int argc, char ** argv) {
    config c;
    for (int i = 1; i < argc; i += 2) {
        check(i + 1 < argc, "each option needs a value");
        std::string key = argv[i], value = argv[i + 1];
        if      (key == "--mode") c.mode = value;
        else if (key == "--device") c.device = value;
        else if (key == "--address") c.address = value;
        else if (key == "--output") c.output = value;
        else if (key == "--port") c.port = number(value, 1024, 65535);
        else if (key == "--warmup") c.warmup = number(value, 0, 100000);
        else if (key == "--iterations") c.iterations = number(value, 1, 1000000);
        else throw std::runtime_error("unknown option: " + key);
    }
    check((c.mode == "client" || c.mode == "server") && c.port && !c.output.empty() && !c.address.empty(),
          "mode, port, address and output are required");
    check(c.device == "CUDA0" || c.device == "Vulkan0", "device must be CUDA0 or Vulkan0");
    const char * enabled = std::getenv("GGML_MCDMA");
    check(enabled && std::string(enabled) == "1", "GGML_MCDMA=1 is required");
    return c;
}
class socket_control {
    int fd_ = -1;
    void wait(short events, timer::time_point deadline) {
        for (;;) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - timer::now()).count();
            check(left > 0, "control deadline expired");
            pollfd p{fd_, events, 0};
            int status = poll(&p, 1, (int) std::min<int64_t>(left, INT32_MAX));
            if (status < 0 && errno == EINTR) continue;
            check(status > 0 && !(p.revents & POLLNVAL), "control poll failed");
            if (p.revents & (events | POLLERR | POLLHUP)) return;
        }
    }
    void io(void * memory, size_t bytes, bool send) {
        auto deadline = timer::now() + std::chrono::seconds(300);
        auto * data = static_cast<uint8_t *>(memory);
        while (bytes) {
            wait(send ? POLLOUT : POLLIN, deadline);
            ssize_t n = send ? ::send(fd_, data, bytes, MSG_NOSIGNAL) : ::recv(fd_, data, bytes, 0);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            check(n > 0, "control peer disconnected");
            data += n; bytes -= (size_t) n;
        }
    }
public:
    explicit socket_control(const config & c) {
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_port = htons((uint16_t) c.port);
        check(inet_pton(AF_INET, c.address.c_str(), &address.sin_addr) == 1, "address must be numeric IPv4");
        fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        check(fd_ >= 0, "socket creation failed");
        try {
            auto deadline = timer::now() + std::chrono::seconds(300);
            if (!c.client()) {
                check(bind(fd_, (sockaddr *) &address, sizeof(address)) == 0 && listen(fd_, 1) == 0, "control listen failed");
                std::cerr << "MCDMA_LATENCY_READY\n";
                wait(POLLIN, deadline);
                int connected = accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                check(connected >= 0, "control accept failed");
                close(fd_); fd_ = connected;
            } else {
                int status = connect(fd_, (sockaddr *) &address, sizeof(address));
                check(status == 0 || errno == EINPROGRESS, "control connect failed");
                if (status) {
                    wait(POLLOUT, deadline);
                    int error = 0; socklen_t length = sizeof(error);
                    check(getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0, "control connect failed");
                }
            }
            int value = 1;
            check(setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)) == 0, "TCP_NODELAY failed");
        } catch (...) { close(fd_); fd_ = -1; throw; }
    }
    ~socket_control() { if (fd_ >= 0) close(fd_); }
    void send(const void * data, size_t bytes) { io(const_cast<void *>(data), bytes, true); }
    void receive(void * data, size_t bytes) { io(data, bytes, false); }
    void put(uint64_t value) {
        uint8_t data[8];
        for (int i = 0; i < 8; ++i) data[i] = (uint8_t) (value >> (8 * i));
        send(data, sizeof(data));
    }
    uint64_t get() {
        uint8_t data[8]; receive(data, sizeof(data));
        uint64_t value = 0;
        for (int i = 0; i < 8; ++i) value |= uint64_t(data[i]) << (8 * i);
        return value;
    }
};

using context_ptr = std::unique_ptr<ggml_context, decltype(&ggml_free)>;
using buffer_ptr = std::unique_ptr<ggml_backend_buffer, decltype(&ggml_backend_buffer_free)>;
using backend_ptr = std::unique_ptr<ggml_backend, decltype(&ggml_backend_free)>;
struct gpu_data {
    context_ptr context{nullptr, ggml_free};
    buffer_ptr buffer{nullptr, ggml_backend_buffer_free};
    ggml_tensor * tx = nullptr, * rx_read = nullptr, * rx_write = nullptr;
    ggml_cgraph * init = nullptr, * verify = nullptr;
    std::vector<ggml_tensor *> errors;
    static constexpr size_t guard = 80;
    gpu_data(ggml_backend_t backend, size_t bytes, bool client) {
        ggml_init_params p{};
        p.no_alloc = true;
        p.mem_size = 192 * ggml_tensor_overhead() + 2 * ggml_graph_overhead_custom(256, false);
        context.reset(ggml_init(p));
        check(bool(context), "ggml context allocation failed");
        auto * ctx = context.get();
        size_t n = bytes / sizeof(float), total = n + 2 * guard;
        float own = client ? 1000000.0f : 2000000.0f;
        float peer = client ? 2000000.0f : 1000000.0f;
        tx = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, total);
        rx_read = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, total);
        rx_write = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, total);
        ggml_set_name(tx, "mcdma-latency-tx");
        ggml_set_name(rx_read, "mcdma-latency-read");
        ggml_set_name(rx_write, "mcdma-latency-write");
        init = ggml_new_graph_custom(ctx, 256, false);
        const std::pair<ggml_tensor *, float> initialization[] = {{tx, own}, {rx_read, -1000000.0f}, {rx_write, -2000000.0f}};
        for (const auto & item : initialization) {
            auto * values = ggml_arange(ctx, item.second, item.second + total, 1);
            ggml_build_forward_expand(init, ggml_cpy(ctx, values, item.first));
        }
        verify = ggml_new_graph_custom(ctx, 256, false);
        auto compare = [&](ggml_tensor * tensor, size_t offset, size_t length, float start) {
            auto * view = ggml_view_1d(ctx, tensor, length, offset * sizeof(float));
            auto * reference = ggml_arange(ctx, start, start + length, 1);
            auto * error = ggml_sum(ctx, ggml_sqr(ctx, ggml_sub(ctx, view, reference)));
            errors.push_back(error); ggml_build_forward_expand(verify, error);
        };
        compare(tx, 0, total, own);
        compare(rx_read, 0, guard, -1000000.0f);
        compare(rx_read, guard + n, guard, -1000000.0f + guard + n);
        compare(rx_read, guard, n, (client ? peer : -1000000.0f) + guard);
        compare(rx_write, 0, guard, -2000000.0f);
        compare(rx_write, guard + n, guard, -2000000.0f + guard + n);
        compare(rx_write, guard, n, (client ? -2000000.0f : peer) + guard);
        buffer.reset(ggml_backend_alloc_ctx_tensors(ctx, backend));
        check(bool(buffer), "GPU tensor allocation failed");
        check(ggml_backend_graph_compute(backend, init) == GGML_STATUS_SUCCESS, "GPU payload generation failed");
    }
    void check_gpu(ggml_backend_t backend) {
        check(ggml_backend_graph_compute(backend, verify) == GGML_STATUS_SUCCESS, "GPU verifier graph failed");
        for (auto * tensor : errors) {
            float error = -1;
            ggml_backend_tensor_get(tensor, &error, 0, sizeof(error));
            check(std::isfinite(error) && error == 0, "GPU payload or guard mismatch");
        }
    }
};

class lease {
    ggml_backend_buffer_t buffer_;
    mcdma_channel & channel_;
    ggml_backend_dma_span span_{};
    bool active_ = false;
    int exceptions_ = std::uncaught_exceptions();
public:
    std::unique_ptr<mcdma_region> tx, read, write;
    double acquire_us = 0, release_us = 0, registration_us = 0;
    lease(mcdma_channel & channel, gpu_data & data, size_t bytes) : buffer_(data.buffer.get()), channel_(channel) {
        check(buffer_->iface.dma_acquire && buffer_->iface.dma_release, "native GPU DMA hooks are unavailable");
        auto start = timer::now();
        check(buffer_->iface.dma_acquire(buffer_, 0, buffer_->size, true, &span_), "GPU DMA acquire failed");
        active_ = true; acquire_us = elapsed_us(start);
        try {
            check(span_.size == buffer_->size, "DMA allocation span mismatch");
            auto region = [&](ggml_tensor * tensor, bool remote_write) {
                uintptr_t base = (uintptr_t) ggml_backend_buffer_get_base(buffer_);
                uintptr_t ptr = (uintptr_t) tensor->data;
                check(ptr >= base && ptr - base <= span_.size, "tensor offset outside allocation");
                size_t offset = ptr - base + gpu_data::guard * sizeof(float);
                check(offset <= span_.size && bytes <= span_.size - offset, "tensor span outside allocation");
                auto subspan = span_;
                subspan.host_ptr = static_cast<uint8_t *>(span_.host_ptr) + offset;
                subspan.size = bytes;
                if (span_.fd >= 0) subspan.fd_offset += offset;
                return channel_.register_span(subspan, remote_write);
            };
            start = timer::now();
            tx = region(data.tx, false); read = region(data.rx_read, false); write = region(data.rx_write, true);
            registration_us = elapsed_us(start);
        } catch (...) { channel_.stop(); release(); throw; }
    }
    void release() {
        if (!active_) return;
        tx.reset(); read.reset(); write.reset();
        auto start = timer::now();
        if (!buffer_->iface.dma_release(buffer_, &span_)) std::abort();
        release_us = elapsed_us(start); active_ = false;
    }
    ~lease() {
        if (std::uncaught_exceptions() > exceptions_) channel_.stop();
        release();
    }
};

struct descriptor { uint64_t tx_address, tx_key, write_address, write_key; };
static descriptor exchange(socket_control & socket, lease & memory, bool client) {
    descriptor peer{};
    auto send = [&]() { socket.put(memory.tx->address()); socket.put(memory.tx->rkey()); socket.put(memory.write->address()); socket.put(memory.write->rkey()); };
    auto recv = [&]() { peer.tx_address = socket.get(); peer.tx_key = socket.get(); peer.write_address = socket.get(); peer.write_key = socket.get(); };
    if (client) { send(); recv(); } else { recv(); send(); }
    check(peer.tx_key <= UINT32_MAX && peer.write_key <= UINT32_MAX, "invalid MR key width");
    return peer;
}
static std::string stats(std::vector<double> values) {
    check(!values.empty(), "latency sample vector is empty");
    std::sort(values.begin(), values.end());
    auto pct = [&](double p) { return values[std::min(values.size() - 1, (size_t) std::ceil(p * values.size()) - 1)]; };
    std::ostringstream out;
    out << std::setprecision(9) << "{\"count\":" << values.size() << ",\"min_us\":" << values.front()
        << ",\"median_us\":" << pct(.5) << ",\"p95_us\":" << pct(.95) << ",\"p99_us\":" << pct(.99)
        << ",\"max_us\":" << values.back() << '}';
    return out.str();
}

int main(int argc, char ** argv) {
    int output_fd = -1;
    try {
        auto c = arguments(argc, argv);
        output_fd = open(c.output.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        check(output_fd >= 0, "output must be a new writable file");
        ggml_backend_load_all();
        auto device = ggml_backend_dev_by_name(c.device.c_str());
        check(device, "selected GPU backend unavailable");
        auto type = ggml_backend_dev_type(device);
        check(type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU, "selected backend is not a GPU");
        backend_ptr backend(ggml_backend_dev_init(device, nullptr), ggml_backend_free);
        check(bool(backend), "GPU initialization failed");
        socket_control socket(c);
        mcdma_channel channel;
        const uint64_t magic = UINT64_C(0x3141544c414d4443);
        const uint16_t endian = 1;
        check(*(const uint8_t *) &endian == 1, "caps exchange requires little endian");
        auto local = channel.caps(); mcdma_caps peer{};
        auto hello_send = [&]() { socket.put(magic); socket.put(c.warmup); socket.put(c.iterations); socket.put(sizeof(local)); socket.send(&local, sizeof(local)); };
        auto hello_recv = [&]() {
            check(socket.get() == magic && socket.get() == (uint64_t) c.warmup && socket.get() == (uint64_t) c.iterations && socket.get() == sizeof(local), "latency configuration or protocol mismatch");
            socket.receive(&peer, sizeof(peer));
        };
        if (c.client()) { hello_send(); hello_recv(); } else { hello_recv(); hello_send(); }
        channel.connect(peer);
        std::ostringstream result;
        result << std::setprecision(9) << "{\"mode\":\"" << c.mode << "\",\"device\":\"" << c.device
               << "\",\"measurement\":\"persistent_mr_nic_completion_only\",\"gpu_between_transfers\":false"
               << ",\"cpu_payload_access\":false,\"warmup\":" << c.warmup << ",\"iterations\":" << c.iterations << ",\"cases\":[";
        bool first = true;
        for (size_t bytes : {size_t(64), size_t(1024), size_t(4096), size_t(65536)}) {
            gpu_data data(backend.get(), bytes, c.client());
            lease memory(channel, data, bytes);
            auto remote = exchange(socket, memory, c.client());
            std::vector<double> read_samples, write_samples;
            if (c.client()) {
                for (bool writing : {false, true}) {
                    auto & samples = writing ? write_samples : read_samples;
                    samples.reserve(c.iterations);
                    auto & region = writing ? *memory.tx : *memory.read;
                    uint64_t address = writing ? remote.write_address : remote.tx_address;
                    uint32_t key = (uint32_t) (writing ? remote.write_key : remote.tx_key);
                    for (int i = 0; i < c.warmup; ++i) channel.transfer(region, address, key, bytes, writing);
                    for (int i = 0; i < c.iterations; ++i) {
                        auto start = timer::now();
                        channel.transfer(region, address, key, bytes, writing);
                        samples.push_back(elapsed_us(start));
                    }
                }
                socket.put(1); check(socket.get() == 1, "server completion ACK failed");
            } else {
                check(socket.get() == 1, "client completion marker failed"); socket.put(1);
            }
            memory.release();
            data.check_gpu(backend.get());
            if (c.client()) { socket.put(2); check(socket.get() == 2, "peer GPU verification failed"); }
            else { check(socket.get() == 2, "peer GPU verification failed"); socket.put(2); }
            result << (first ? "" : ",") << "{\"bytes\":" << bytes << ",\"gpu_guard_check\":true"
                   << ",\"acquire_batch_us\":" << memory.acquire_us << ",\"registration_batch_us\":" << memory.registration_us
                   << ",\"release_batch_us\":" << memory.release_us;
            if (c.client()) {
                result << ",\"read\":" << stats(read_samples) << ",\"write\":" << stats(write_samples);
                const std::pair<const char *, const std::vector<double> *> records[] = {
                    {"read_samples_us", &read_samples}, {"write_samples_us", &write_samples}};
                for (const auto & item : records) {
                    result << ",\"" << item.first << "\":[";
                    for (size_t i = 0; i < item.second->size(); ++i) result << (i ? "," : "") << (*item.second)[i];
                    result << ']';
                }
            }
            result << '}'; first = false;
            std::cout << "MCDMA_NATIVE_LATENCY_CASE_PASS bytes=" << bytes << " nic_only=1 gpu_between_transfers=0\n";
        }
        channel.stop();
        result << "]}\n";
        std::string text = result.str(); size_t offset = 0;
        while (offset < text.size()) {
            ssize_t n = ::write(output_fd, text.data() + offset, text.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            check(n > 0, "result write failed"); offset += (size_t) n;
        }
        check(fsync(output_fd) == 0, "result fsync failed"); close(output_fd); output_fd = -1;
        std::cout << "MCDMA_NATIVE_LATENCY_PASS cases=4 measurement=persistent_mr_nic_completion_only\n";
        return 0;
    } catch (const std::exception & error) {
        if (output_fd >= 0) close(output_fd);
        std::cerr << "MCDMA_NATIVE_LATENCY_FAIL: " << error.what() << '\n';
        return 1;
    }
}
