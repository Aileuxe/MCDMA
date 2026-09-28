// Private pinned llama.cpp integration: CPU metadata and direct GPU KV RDMA.
#include "llama.h"
#include "llama-io.h"
#include "llama-memory.h"
#include "mcdma-rdma.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using clock_type = std::chrono::steady_clock;
static void require(bool ok, const char * message) { if (!ok) throw std::runtime_error(message); }
static double seconds(clock_type::time_point start) {
    return std::chrono::duration<double>(clock_type::now() - start).count();
}

struct options {
    std::string mode, model, tokens, device, address, output, model_id;
    int port = 0, n_ctx = 16384, predict = 128, timeout = 300;
};

static int parse_int(const std::string & text, int low, int high) {
    size_t end = 0;
    long value = std::stol(text, &end);
    require(end == text.size() && value >= low && value <= high, "integer argument outside accepted range");
    return (int) value;
}

static options parse(int argc, char ** argv) {
    options o;
    for (int i = 1; i < argc; i += 2) {
        require(i + 1 < argc, "each option needs a value");
        std::string key = argv[i], value = argv[i + 1];
        if      (key == "--mode") o.mode = value;
        else if (key == "--model") o.model = value;
        else if (key == "--tokens") o.tokens = value;
        else if (key == "--device") o.device = value;
        else if (key == "--address") o.address = value;
        else if (key == "--output") o.output = value;
        else if (key == "--model-id") o.model_id = value;
        else if (key == "--port") o.port = parse_int(value, 1024, 65535);
        else if (key == "--ctx") o.n_ctx = parse_int(value, 512, 131072);
        else if (key == "--predict") o.predict = parse_int(value, 1, 4096);
        else if (key == "--timeout") o.timeout = parse_int(value, 1, 3600);
        else throw std::runtime_error("unknown option: " + key);
    }
    require(o.mode == "send" || o.mode == "receive" || o.mode == "baseline", "mode must be send, receive or baseline");
    require(!o.model.empty() && !o.tokens.empty() && !o.device.empty() && !o.output.empty(), "model, tokens, device and output are required");
    require(o.device == "CUDA0" || o.device == "Vulkan0", "this bounded test requires CUDA0 or Vulkan0");
    require(o.mode == "baseline" || (o.mode == "send" && o.device == "CUDA0") ||
            (o.mode == "receive" && o.device == "Vulkan0"), "handoff requires a CUDA donor and Vulkan receiver");
    require(o.mode == "baseline" || (!o.address.empty() && o.port), "send/receive require address and port");
    require(o.model_id.size() == 64 && o.model_id.find_first_not_of("0123456789abcdef") == std::string::npos,
            "model-id must be the independently verified lowercase model SHA-256");
    const char * enabled = std::getenv("GGML_MCDMA");
    require(enabled && std::string(enabled) == "1", "GGML_MCDMA=1 is required; no allocator fallback is allowed");
    return o;
}

class control_socket {
    int fd_ = -1;
    int timeout_;
    void wait(short events, clock_type::time_point deadline) {
        while (true) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock_type::now()).count();
            require(left > 0, "control deadline expired");
            pollfd item{fd_, events, 0};
            int n = ::poll(&item, 1, (int) std::min<int64_t>(left, INT32_MAX));
            if (n < 0 && errno == EINTR) continue;
            require(n > 0 && !(item.revents & POLLNVAL), "control connection failed or timed out");
            if (item.revents & (events | POLLERR | POLLHUP)) return;
        }
    }
public:
    control_socket(const options & o) : timeout_(o.timeout) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons((uint16_t) o.port);
        require(inet_pton(AF_INET, o.address.c_str(), &address.sin_addr) == 1, "address must be numeric IPv4");
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        require(fd_ >= 0, "socket creation failed");
        try {
            auto deadline = clock_type::now() + std::chrono::seconds(timeout_);
            if (o.mode == "receive") {
                require(::bind(fd_, (sockaddr *) &address, sizeof(address)) == 0, "control bind failed");
                require(::listen(fd_, 1) == 0, "control listen failed");
                std::cerr << "MCDMA_KV_READY\n";
                wait(POLLIN, deadline);
                int client = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
                require(client >= 0, "control accept failed");
                ::close(fd_);
                fd_ = client;
            } else {
                int status = ::connect(fd_, (sockaddr *) &address, sizeof(address));
                require(status == 0 || errno == EINPROGRESS, "control connect failed");
                if (status) {
                    wait(POLLOUT, deadline);
                    int error = 0;
                    socklen_t length = sizeof(error);
                    require(getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0, "control connect failed");
                }
            }
            int no_delay = 1;
            require(setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) == 0,
                    "cannot disable control socket Nagle delay");
        } catch (...) { ::close(fd_); fd_ = -1; throw; }
    }
    ~control_socket() { if (fd_ >= 0) ::close(fd_); }
    void bytes(void * data, size_t size, bool sending) {
        auto deadline = clock_type::now() + std::chrono::seconds(timeout_);
        auto * cursor = static_cast<uint8_t *>(data);
        while (size) {
            wait(sending ? POLLOUT : POLLIN, deadline);
            ssize_t n = sending ? ::send(fd_, cursor, size, MSG_NOSIGNAL) : ::recv(fd_, cursor, size, 0);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            require(n > 0, "control socket disconnected");
            cursor += n;
            size -= (size_t) n;
        }
    }
    void send(const void * data, size_t size) { bytes(const_cast<void *>(data), size, true); }
    void recv(void * data, size_t size) { bytes(data, size, false); }
    void put(uint64_t value) {
        uint8_t data[8];
        for (int i = 0; i < 8; ++i) data[i] = (uint8_t) (value >> (i * 8));
        send(data, sizeof(data));
    }
    uint64_t get() {
        uint8_t data[8]; recv(data, sizeof(data));
        uint64_t value = 0;
        for (int i = 0; i < 8; ++i) value |= uint64_t(data[i]) << (i * 8);
        return value;
    }
};

struct counts { uint64_t metadata = 0, tensor = 0, spans = 0; };

struct kv_owner {
    std::shared_ptr<mcdma_channel> channel;
    std::atomic<uint64_t> registrations{0}, registered_bytes{0};
    explicit kv_owner(std::shared_ptr<mcdma_channel> value) : channel(std::move(value)) {}
};

struct kv_cache {
    // Member order keeps the channel alive until the MR has been deregistered.
    std::shared_ptr<kv_owner> owner;
    std::unique_ptr<mcdma_region> region;
    void * host_pointer = nullptr;
    uint64_t fd_offset = 0;
};

static std::weak_ptr<kv_owner> observing_owner;
static void cleanup_kv_cache(void * pointer) {
    auto * cache = static_cast<kv_cache *>(pointer);
    if (std::uncaught_exceptions()) cache->owner->channel->stop();
    delete cache;
}

static void observe_kv_buffer(ggml_backend_buffer_t buffer) {
    auto owner = observing_owner.lock();
    if (!owner) return;
    try {
        require(!buffer->dma_owner && !buffer->dma_cleanup, "GPU buffer already has a DMA cache owner");
        ggml_backend_dma_span span{};
        span.fd = -1;
        require(buffer->iface.dma_export && buffer->iface.dma_export(buffer, &span), "GPU allocation export failed");
        try {
            require(span.size == buffer->size, "GPU allocation export length mismatch");
            auto cache = std::make_unique<kv_cache>();
            cache->owner = owner;
            cache->host_pointer = span.host_ptr;
            cache->fd_offset = span.fd_offset;
            cache->region = owner->channel->register_span(span, false);
            if (span.fd >= 0) {
                int fd = span.fd; span.fd = -1;
                require(close(fd) == 0, "GPU export descriptor close failed");
            }
            buffer->dma_owner = cache.release();
            buffer->dma_cleanup = cleanup_kv_cache;
            ++owner->registrations;
            owner->registered_bytes += buffer->size;
        } catch (...) {
            if (span.fd >= 0) close(span.fd);
            throw;
        }
    } catch (const std::exception & error) {
        owner->channel->stop();
        std::cerr << "MCDMA KV allocation observer failed: " << error.what() << '\n';
        std::abort();
    }
}

class kv_observer {
    std::shared_ptr<kv_owner> owner_;
public:
    explicit kv_observer(std::shared_ptr<kv_owner> owner) : owner_(std::move(owner)) {
        require(observing_owner.expired(), "only one native KV observer is supported");
        observing_owner = owner_;
        ggml_backend_buffer_set_dma_observer(observe_kv_buffer);
    }
    ~kv_observer() {
        ggml_backend_buffer_set_dma_observer(nullptr);
        observing_owner.reset();
    }
};

static kv_cache & cached_tensor(ggml_tensor * tensor, size_t offset, size_t size,
                               const std::shared_ptr<kv_owner> & owner, size_t & allocation_offset) {
    auto buffer = tensor->buffer;
    require(buffer && buffer->dma_cleanup == cleanup_kv_cache && buffer->dma_owner,
            "KV tensor has no cold-registered native GPU buffer");
    auto * cache = static_cast<kv_cache *>(buffer->dma_owner);
    require(cache->owner == owner && cache->region && cache->region->size() == buffer->size,
            "KV tensor cache owner or allocation mismatch");
    require(size && offset <= ggml_nbytes(tensor) && size <= ggml_nbytes(tensor) - offset,
            "KV tensor span outside tensor");
    uintptr_t base = (uintptr_t) ggml_backend_buffer_get_base(buffer);
    uintptr_t data = (uintptr_t) tensor->data;
    require(data >= base && data - base <= buffer->size && offset <= buffer->size - (data - base) &&
            size <= buffer->size - (data - base) - offset, "KV tensor span outside backing allocation");
    allocation_offset = data - base + offset;
    return *cache;
}

static constexpr size_t max_metadata_bytes = 16 * 1024 * 1024;

class dma_writer final : public llama_io_write_i {
    control_socket & socket_;
    std::shared_ptr<kv_owner> owner_;
    std::vector<uint8_t> metadata_;
    int exceptions_ = std::uncaught_exceptions();
    void append(uint64_t value) {
        require(metadata_.size() <= max_metadata_bytes - 8, "state metadata exceeds protocol cap");
        for (int i = 0; i < 8; ++i) metadata_.push_back((uint8_t) (value >> (i * 8)));
    }
public:
    counts count;
    dma_writer(control_socket & socket, std::shared_ptr<kv_owner> owner) : socket_(socket), owner_(std::move(owner)) {}
    ~dma_writer() { if (std::uncaught_exceptions() > exceptions_) owner_->channel->stop(); }
    void write(const void * src, size_t size) override {
        require(size <= max_metadata_bytes && metadata_.size() <= max_metadata_bytes - size &&
                max_metadata_bytes - metadata_.size() - size >= 16, "metadata record exceeds protocol cap");
        append(1); append(size);
        const auto * bytes = static_cast<const uint8_t *>(src);
        if (size) metadata_.insert(metadata_.end(), bytes, bytes + size);
        count.metadata += size;
    }
    void flush() {
        socket_.put(5); socket_.put(metadata_.size());
        socket_.send(metadata_.data(), metadata_.size());
        metadata_.clear();
    }
    void write_tensor(ggml_tensor * tensor, size_t offset, size_t size) override {
        size_t allocation_offset = 0;
        auto & cache = cached_tensor(tensor, offset, size, owner_, allocation_offset);
        require(allocation_offset <= UINT64_MAX - cache.region->address(), "KV source MR address overflow");
        append(2); append(size);
        append(cache.region->address() + allocation_offset); append(cache.region->rkey());
        count.tensor += size; ++count.spans;
    }
    size_t n_bytes() override { return count.metadata + count.tensor; }
};

class dma_reader final : public llama_io_read_i {
    control_socket & socket_;
    std::shared_ptr<kv_owner> owner_;
    std::vector<uint8_t> metadata_;
    size_t cursor_ = 0;
    struct buffer_lease { ggml_backend_buffer_t buffer; ggml_backend_dma_span span; };
    std::vector<buffer_lease> leases_;
    struct read_request { size_t local_offset, size; uint64_t remote_address; uint32_t remote_key; };
    struct buffer_group {
        ggml_backend_buffer_t buffer;
        kv_cache * cache;
        std::vector<read_request> requests;
    };
    // The enclosing llama_context owns these buffers through finish(); owner_ retains the channel.
    std::vector<buffer_group> groups_;
    int exceptions_ = std::uncaught_exceptions();
    uint64_t get() {
        require(cursor_ <= metadata_.size() && metadata_.size() - cursor_ >= 8, "truncated metadata transcript");
        uint64_t value = 0;
        for (int i = 0; i < 8; ++i) value |= uint64_t(metadata_[cursor_++]) << (i * 8);
        return value;
    }
    void fail() {
        owner_->channel->stop();
        release();
    }
    void report_failure(const char * phase, const char * reason, const ggml_tensor * tensor = nullptr,
                        size_t offset = 0, size_t size = 0) const {
        std::cerr << "MCDMA_KV_CALLBACK_FAIL phase=" << phase << " reason=" << reason
                  << " transcript_offset=" << cursor_ << " transcript_bytes=" << metadata_.size()
                  << " declared_spans=" << count.spans << " declared_kv_bytes=" << count.tensor
                  << " completed_spans=" << completed_spans << " completed_kv_bytes=" << completed_bytes
                  << " held_buffers=" << leases_.size();
        if (tensor) {
            std::cerr << " tensor=" << tensor->name << " offset=" << offset << " bytes=" << size
                      << " allocation_bytes=" << (tensor->buffer ? tensor->buffer->size : 0);
        }
        std::cerr << '\n';
    }
    void acquire(ggml_backend_buffer_t buffer, const kv_cache & cache) {
        for (const auto & lease : leases_) if (lease.buffer == buffer) return;
        require(leases_.empty(), "previous KV allocation is still leased");
        require(buffer->iface.dma_acquire && buffer->iface.dma_release, "KV allocation lacks DMA ownership hooks");
        leases_.push_back({buffer, {nullptr, 0, 0, -1, nullptr}});
        auto & held = leases_.back();
        if (!buffer->iface.dma_acquire(buffer, 0, buffer->size, true, &held.span)) {
            leases_.pop_back();
            throw std::runtime_error("whole KV buffer DMA acquire failed");
        }
        require(held.span.size == buffer->size && held.span.host_ptr == cache.host_pointer &&
                held.span.fd_offset == cache.fd_offset, "KV ownership span differs from cold MR allocation");
        ++ownership_buffers;
    }
public:
    counts count;
    uint64_t ownership_buffers = 0;
    uint64_t completed_spans = 0, completed_bytes = 0;
    dma_reader(control_socket & socket, std::shared_ptr<kv_owner> owner) : socket_(socket), owner_(std::move(owner)) {
        require(socket_.get() == 5, "expected metadata transcript");
        uint64_t size = socket_.get();
        require(size && size <= max_metadata_bytes, "metadata transcript exceeds protocol cap");
        metadata_.resize((size_t) size);
        socket_.recv(metadata_.data(), metadata_.size());
    }
    ~dma_reader() {
        if (std::uncaught_exceptions() > exceptions_) owner_->channel->stop();
        release();
    }
    void release() {
        for (auto & lease : leases_) {
            if (!lease.buffer->iface.dma_release(lease.buffer, &lease.span)) std::abort();
        }
        leases_.clear();
    }
    void finish() {
        try {
            require(cursor_ == metadata_.size(), "unconsumed metadata transcript bytes");
            for (const auto & group : groups_) {
                require(group.buffer->dma_cleanup == cleanup_kv_cache && group.buffer->dma_owner == group.cache &&
                        group.cache->owner == owner_, "queued KV allocation ownership changed");
                acquire(group.buffer, *group.cache);
                for (const auto & request : group.requests) {
                    owner_->channel->transfer_at(*group.cache->region, request.local_offset,
                                                request.remote_address, request.remote_key, request.size, false);
                    completed_bytes += request.size;
                    ++completed_spans;
                }
                release();
            }
            require(completed_bytes == count.tensor && completed_spans == count.spans,
                    "completed KV RDMA operations differ from declared state spans");
            groups_.clear();
        } catch (const std::exception & error) {
            report_failure("finish", error.what()); fail(); throw;
        } catch (...) { report_failure("finish", "unknown exception"); fail(); throw; }
    }
    void read(void * dst, size_t size) override {
        try {
            require(size <= max_metadata_bytes, "metadata read exceeds protocol cap");
            uint64_t kind = get(), declared_size = get();
            if (kind != 1 || declared_size != size) {
                std::ostringstream error;
                error << "metadata framing mismatch: kind=" << kind << " declared_bytes=" << declared_size
                      << " requested_bytes=" << size;
                throw std::runtime_error(error.str());
            }
            require(cursor_ <= metadata_.size() && size <= metadata_.size() - cursor_, "truncated metadata bytes");
            if (size) std::memcpy(dst, metadata_.data() + cursor_, size);
            cursor_ += size; count.metadata += size;
        } catch (const std::exception & error) {
            report_failure("metadata", error.what(), nullptr, 0, size); fail(); throw;
        } catch (...) { report_failure("metadata", "unknown exception"); fail(); throw; }
    }
    void read_tensor(ggml_tensor * tensor, size_t offset, size_t size) override {
        try {
            uint64_t kind = get(), declared_size = get();
            if (kind != 2 || declared_size != size) {
                std::ostringstream error;
                error << "KV span layout mismatch: kind=" << kind << " declared_bytes=" << declared_size
                      << " requested_bytes=" << size;
                throw std::runtime_error(error.str());
            }
            uint64_t address = get(), rkey = get();
            require(rkey <= UINT32_MAX && size <= UINT64_MAX - address, "invalid KV descriptor");
            size_t allocation_offset = 0;
            auto & cache = cached_tensor(tensor, offset, size, owner_, allocation_offset);
            auto group = std::find_if(groups_.begin(), groups_.end(),
                                     [&](const buffer_group & item) { return item.buffer == tensor->buffer; });
            if (group == groups_.end()) {
                groups_.push_back({tensor->buffer, &cache, {}});
                group = groups_.end() - 1;
            }
            require(group->cache == &cache, "KV buffer cache changed while reading metadata");
            group->requests.push_back({allocation_offset, size, address, (uint32_t) rkey});
            count.tensor += size; ++count.spans;
        } catch (const std::exception & error) {
            report_failure("tensor", error.what(), tensor, offset, size); fail(); throw;
        } catch (...) { report_failure("tensor", "unknown exception", tensor, offset, size); fail(); throw; }
    }
    void discard() override { fail(); }
    size_t n_bytes() override { return count.metadata + count.tensor; }
};

static std::vector<llama_token> load_tokens(const std::string & path) {
    std::ifstream input(path);
    require(bool(input), "cannot open token-ID file");
    std::vector<llama_token> result;
    int64_t token;
    while (input >> token) {
        require(token >= 0 && token <= INT32_MAX && result.size() < 131072, "invalid token-ID input");
        result.push_back((llama_token) token);
    }
    require(input.eof() && !result.empty(), "tokens must be whitespace-separated integer IDs");
    return result;
}

static void handshake(control_socket & socket, mcdma_channel & channel, const options & o,
                      const std::vector<llama_token> & tokens) {
    // Both tested Linux architectures are little endian; the caps ABI is versioned.
    const uint16_t endian = 1;
    require(*(const uint8_t *) &endian == 1, "this protocol requires little-endian hosts");
    const uint64_t magic = 0x3144564b414d4443ULL;
    auto local = channel.caps();
    if (o.mode == "send") {
        socket.put(magic); socket.put(2); socket.put(o.n_ctx); socket.put(tokens.size());
        socket.send(o.model_id.data(), 64);
        socket.send(tokens.data(), tokens.size() * sizeof(llama_token));
        socket.put(sizeof(local)); socket.send(&local, sizeof(local));
        require(socket.get() == magic, "receiver handshake mismatch");
        mcdma_caps remote{}; socket.recv(&remote, sizeof(remote)); channel.connect(remote);
    } else {
        require(socket.get() == magic && socket.get() == 2 && socket.get() == (uint64_t) o.n_ctx &&
                socket.get() == tokens.size(), "handoff version, context or prompt length mismatch");
        std::string model_id(64, '\0'); socket.recv(model_id.data(), model_id.size());
        require(model_id == o.model_id, "handoff model identity mismatch");
        std::vector<llama_token> remote_tokens(tokens.size());
        socket.recv(remote_tokens.data(), remote_tokens.size() * sizeof(llama_token));
        require(remote_tokens == tokens, "handoff prompt token mismatch");
        require(socket.get() == sizeof(local), "RDMA caps ABI mismatch");
        mcdma_caps remote{}; socket.recv(&remote, sizeof(remote)); channel.connect(remote);
        socket.put(magic); socket.send(&local, sizeof(local));
    }
}

static void decode_batch(llama_context * ctx, const llama_token * tokens, int count, int position, bool logits) {
    llama_batch batch = llama_batch_init(count, 0, 1);
    require(batch.token != nullptr, "batch allocation failed");
    batch.n_tokens = count;
    for (int i = 0; i < count; ++i) {
        batch.token[i] = tokens[i]; batch.pos[i] = position + i;
        batch.n_seq_id[i] = 1; batch.seq_id[i][0] = 0;
        batch.logits[i] = logits && i == count - 1;
    }
    int result = llama_decode(ctx, batch);
    llama_batch_free(batch);
    require(result == 0, "model decode failed");
}

static std::vector<llama_token> generate(llama_context * ctx, int count, int position, double & first_token) {
    std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> sampler(llama_sampler_init_greedy(), llama_sampler_free);
    require(bool(sampler), "sampler creation failed");
    std::vector<llama_token> output;
    auto start = clock_type::now();
    for (int i = 0; i < count; ++i) {
        llama_token token = llama_sampler_sample(sampler.get(), ctx, -1);
        output.push_back(token);
        if (i == 0) first_token = seconds(start);
        if (i + 1 < count) decode_batch(ctx, &token, 1, position + i, true);
    }
    llama_synchronize(ctx);
    return output;
}

int main(int argc, char ** argv) {
    int output_fd = -1;
    try {
        options o = parse(argc, argv);
        auto tokens = load_tokens(o.tokens);
        require(tokens.size() + o.predict < (size_t) o.n_ctx, "context is too small for prompt and output");
        output_fd = ::open(o.output.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        require(output_fd >= 0, "output must be a new writable file");
        auto setup_start = clock_type::now();
        std::unique_ptr<control_socket> socket;
        std::shared_ptr<kv_owner> owner;
        std::unique_ptr<kv_observer> observer;
        if (o.mode != "baseline") {
            socket = std::make_unique<control_socket>(o);
            owner = std::make_shared<kv_owner>(std::make_shared<mcdma_channel>());
            handshake(*socket, *owner->channel, o, tokens);
            observer = std::make_unique<kv_observer>(owner);
        }
        ggml_backend_load_all();
        llama_backend_init();
        auto device = ggml_backend_dev_by_name(o.device.c_str());
        require(device != nullptr, "requested hardware GPU unavailable");
        auto device_type = ggml_backend_dev_type(device);
        require(device_type == GGML_BACKEND_DEVICE_TYPE_GPU || device_type == GGML_BACKEND_DEVICE_TYPE_IGPU,
                "requested device is not a GPU");
        ggml_backend_dev_t devices[] = {device, nullptr};
        auto mp = llama_model_default_params();
        mp.devices = devices; mp.n_gpu_layers = -1; mp.split_mode = LLAMA_SPLIT_MODE_NONE;
        std::unique_ptr<llama_model, decltype(&llama_model_free)> model(llama_model_load_from_file(o.model.c_str(), mp), llama_model_free);
        require(bool(model), "model loading failed");
        char architecture[64] = {};
        require(llama_model_meta_val_str(model.get(), "general.architecture", architecture, sizeof(architecture)) > 0 &&
                std::string(architecture) == "qwen3", "this bounded handoff runner supports Qwen3 attention caches");
        const auto * vocab = llama_model_get_vocab(model.get());
        for (auto token : tokens) require(token < llama_vocab_n_tokens(vocab), "token exceeds model vocabulary");
        auto cp = llama_context_default_params();
        cp.n_ctx = o.n_ctx; cp.n_batch = 512; cp.n_ubatch = 512; cp.n_seq_max = 1;
        cp.n_threads = 8; cp.n_threads_batch = 8;
        cp.type_k = GGML_TYPE_F16; cp.type_v = GGML_TYPE_F16;
        cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
        cp.offload_kqv = true; cp.no_perf = false;
        std::unique_ptr<llama_context, decltype(&llama_free)> ctx(llama_init_from_model(model.get(), cp), llama_free);
        require(bool(ctx) && llama_get_memory(ctx.get()), "model context or KV cache unavailable");
        const double setup = seconds(setup_start);
        double prefill = 0, handoff = 0, reconstruct = 0, first_token = 0;
        counts traffic;
        uint64_t ownership_buffers = 0, registrations_before_handoff = 0, initiated_rdma_bytes = 0;
        if (o.mode != "receive") {
            auto start = clock_type::now();
            for (size_t pos = 0; pos < tokens.size(); pos += 512) {
                int n = (int) std::min<size_t>(512, tokens.size() - pos);
                decode_batch(ctx.get(), tokens.data() + pos, n, (int) pos, pos + n == tokens.size());
            }
            llama_synchronize(ctx.get());
            std::atomic_thread_fence(std::memory_order_seq_cst);
            prefill = seconds(start);
        }
        if (o.mode != "baseline") {
            if (o.mode == "send") {
                socket->put(6);
                require(socket->get() == 6, "receiver setup boundary failed");
            } else {
                llama_synchronize(ctx.get());
                require(socket->get() == 6, "donor prefill boundary failed");
                socket->put(6);
            }
            registrations_before_handoff = owner->registrations.load();
            auto start = clock_type::now();
            if (o.mode == "send") {
                dma_writer writer(*socket, owner);
                llama_get_memory(ctx.get())->state_write(writer, 0, 0);
                writer.flush();
                socket->put(3); socket->put(writer.n_bytes());
                require(socket->get() == 4, "receiver did not finish restoring state");
                traffic = writer.count;
            } else {
                dma_reader reader(*socket, owner);
                llama_get_memory(ctx.get())->state_read(reader, 0, 0);
                reader.finish();
                require(socket->get() == 3 && socket->get() == reader.n_bytes(), "state completion framing mismatch");
                require(llama_memory_seq_pos_min(llama_get_memory(ctx.get()), 0) == 0 &&
                        llama_memory_seq_pos_max(llama_get_memory(ctx.get()), 0) == (llama_pos) tokens.size() - 1,
                        "restored cache positions differ from prompt");
                socket->put(4);
                traffic = reader.count;
                ownership_buffers = reader.ownership_buffers;
                initiated_rdma_bytes = reader.completed_bytes;
            }
            require(traffic.spans > 0 && traffic.tensor > 0, "no GPU KV payload was transferred");
            require(owner->registrations.load() == registrations_before_handoff, "unexpected registration inside KV handoff");
            owner->channel->stop();
            handoff = seconds(start);
            observer.reset();
        }
        if (o.mode == "receive") {
            auto start = clock_type::now();
            require(llama_memory_seq_rm(llama_get_memory(ctx.get()), 0, (llama_pos) tokens.size() - 1, -1), "cannot remove final cached token");
            decode_batch(ctx.get(), &tokens.back(), 1, (int) tokens.size() - 1, true);
            llama_synchronize(ctx.get());
            reconstruct = seconds(start);
        }
        auto start = clock_type::now();
        auto generated = generate(ctx.get(), o.predict, (int) tokens.size(), first_token);
        double decode = seconds(start);
        auto perf = llama_perf_context(ctx.get());
        std::ostringstream out;
        out << std::setprecision(9) << "{\"mode\":\"" << o.mode << "\",\"device\":\"" << o.device
            << "\",\"model_sha256_declared\":\"" << o.model_id << "\",\"prompt_tokens\":" << tokens.size()
            << ",\"cold_setup_wall_s\":" << setup
            << ",\"generated_tokens\":" << generated.size() << ",\"prefill_s\":" << prefill
            << ",\"handoff_s\":" << handoff << ",\"reconstruct_logits_s\":" << reconstruct
            << ",\"decode_s\":" << decode << ",\"decode_first_token_s\":" << first_token
            << ",\"metadata_bytes\":" << traffic.metadata << ",\"rdma_kv_bytes\":" << traffic.tensor
            << ",\"rdma_kv_spans\":" << traffic.spans << ",\"kv_payload_staging_bytes\":0"
            << ",\"registrations_before_handoff\":" << registrations_before_handoff
            << ",\"registrations_during_handoff\":0,\"receiver_ownership_buffers\":" << ownership_buffers
            << ",\"locally_initiated_rdma_bytes\":" << initiated_rdma_bytes
            << ",\"per_span_control_acks\":0,\"protocol_version\":2"
            << ",\"n_p_eval\":" << perf.n_p_eval << ",\"n_eval\":" << perf.n_eval << ",\"tokens\":[";
        for (size_t i = 0; i < generated.size(); ++i) out << (i ? "," : "") << generated[i];
        out << "]}\n";
        std::string result = out.str();
        size_t written = 0;
        while (written < result.size()) {
            ssize_t n = ::write(output_fd, result.data() + written, result.size() - written);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "result write failed"); written += (size_t) n;
        }
        require(::fsync(output_fd) == 0, "result fsync failed");
        ::close(output_fd); output_fd = -1;
        std::cout << "MCDMA_NATIVE_KV_PASS mode=" << o.mode << " tokens=" << generated.size()
                  << " rdma_kv_bytes=" << traffic.tensor << " spans=" << traffic.spans << '\n';
        return 0;
    } catch (const std::exception & error) {
        if (output_fd >= 0) ::close(output_fd);
        std::cerr << "MCDMA_NATIVE_KV_FAIL: " << error.what() << '\n';
        return 1;
    }
}
