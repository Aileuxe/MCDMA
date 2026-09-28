// One-host timing from GPU producer submission through remote GPU consumption.
#include "ggml.h"
#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml-backend-impl.h"
#include "ggml-rpc.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using clock_type = std::chrono::steady_clock;
using backend_ptr = std::unique_ptr<ggml_backend, decltype(&ggml_backend_free)>;
using context_ptr = std::unique_ptr<ggml_context, decltype(&ggml_free)>;
using buffer_ptr = std::unique_ptr<ggml_backend_buffer, decltype(&ggml_backend_buffer_free)>;

static void require(bool ok, const char * message) {
    if (!ok) throw std::runtime_error(message);
}
static double us(clock_type::time_point begin, clock_type::time_point end) {
    return std::chrono::duration<double, std::micro>(end - begin).count();
}
struct config {
    std::string rpc, output;
    unsigned warmup = 100, iterations = 1000, timeout_seconds = 300;
    double require_e2e_us = 0;
    bool negative = false;
};
static unsigned integer(const std::string & value, unsigned minimum, unsigned maximum) {
    size_t end = 0;
    unsigned long result = std::stoul(value, &end);
    require(end == value.size() && result >= minimum && result <= maximum, "integer option out of range");
    return static_cast<unsigned>(result);
}
static void usage() {
    std::cout << "mcdma-native-e2e --rpc HOST:PORT --output NEW_JSON [--warmup 100] [--iterations 1000] "
                 "[--timeout-seconds 300] [--require-e2e-us 10] [--negative]\n"
                 "The gate requires every case's observed maximum producer-to-consumer latency to be strictly below the limit.\n"
                 "The receiver is GPU-ready before each producer starts; verification/rearming and full cycle times are separate.\n"
                 "Exit codes: 0 pass, 1 runtime failure, 2 latency gate failure, 3 GPU correctness failure.\n";
}
static config arguments(int argc, char ** argv) {
    config c;
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        if (key == "--negative") { c.negative = true; continue; }
        if (key == "--require-e2e-us10") { c.require_e2e_us = 10; continue; }
        require(i + 1 < argc, "option requires a value");
        std::string value = argv[++i];
        if (key == "--rpc") c.rpc = value;
        else if (key == "--output") c.output = value;
        else if (key == "--warmup") c.warmup = integer(value, 0, 10000);
        else if (key == "--iterations") c.iterations = integer(value, 1, 100000);
        else if (key == "--timeout-seconds") c.timeout_seconds = integer(value, 1, 3600);
        else if (key == "--require-e2e-us") {
            size_t end = 0;
            c.require_e2e_us = std::stod(value, &end);
            require(end == value.size() && std::isfinite(c.require_e2e_us) && c.require_e2e_us > 0 && c.require_e2e_us <= 1000000,
                    "invalid latency threshold");
        } else throw std::runtime_error("unknown option");
    }
    require(!c.rpc.empty() && !c.output.empty(), "--rpc and --output are required");
    const char * enabled = std::getenv("GGML_MCDMA");
    require(enabled && std::string(enabled) == "1", "GGML_MCDMA=1 is required");
    return c;
}

static void compute(ggml_backend_t backend, ggml_cgraph * graph) {
    require(ggml_backend_graph_compute(backend, graph) == GGML_STATUS_SUCCESS, "GPU graph failed");
}
static void plan(ggml_tensor * tensor) {
    auto * buffer = tensor->buffer;
    require(buffer && buffer->iface.dma_plan, "Vulkan DMA planning hook unavailable");
    const uintptr_t base = reinterpret_cast<uintptr_t>(ggml_backend_buffer_get_base(buffer));
    const uintptr_t data = reinterpret_cast<uintptr_t>(tensor->data);
    const size_t length = ggml_nbytes(tensor);
    require(data >= base && data - base <= buffer->size && length <= buffer->size - (data - base), "planned range exceeds buffer");
    require(buffer->iface.dma_plan(buffer, data - base, length), "Vulkan DMA plan failed");
}

struct gpu_fixture {
    static constexpr size_t prefix = 80, suffix = 96;
    context_ptr context{nullptr, ggml_free};
    buffer_ptr buffer{nullptr, ggml_backend_buffer_free};
    ggml_tensor * values = nullptr, * payload = nullptr;
    ggml_cgraph * initialize = nullptr, * step = nullptr, * verify = nullptr;
    std::array<ggml_tensor *, 3> errors{};

    gpu_fixture(ggml_backend_t backend, size_t bytes, bool producer, bool negative) {
        ggml_init_params params{};
        params.no_alloc = true;
        params.mem_size = 128 * ggml_tensor_overhead() + 3 * ggml_graph_overhead_custom(128, false);
        context.reset(ggml_init(params));
        require(bool(context), "GPU fixture metadata allocation failed");
        auto * ctx = context.get();
        const size_t count = bytes / sizeof(float), length = prefix + count + suffix;
        auto tensor = [&](size_t size) { return ggml_new_tensor_1d(ctx, GGML_TYPE_F32, static_cast<int64_t>(size)); };
        auto graph = [&]() {
            return ggml_new_graph_custom(ctx, 128, false);
        };
        initialize = graph(); step = graph(); verify = graph();
        values = tensor(length);
        payload = ggml_view_1d(ctx, values, count, prefix * sizeof(float));
        ggml_set_name(values, producer ? "mcdma-e2e-producer" : "mcdma-e2e-consumer");
        ggml_set_name(payload, producer ? "mcdma-e2e-tx-span" : "mcdma-e2e-rx-span");
        auto * expected_payload = tensor(count);
        auto * expected_prefix = tensor(prefix);
        auto * expected_suffix = tensor(suffix);
        const float source_start = 1000000.0f;
        const float guard_start = producer ? source_start : -2000000.0f;
        auto range = [&](float start, size_t size) { return ggml_arange(ctx, start, start + static_cast<float>(size), 1.0f); };
        auto initialize_tensor = [&](ggml_tensor * destination, ggml_tensor * initial) {
            ggml_build_forward_expand(initialize, ggml_cpy(ctx, initial, destination));
        };
        initialize_tensor(values, range(guard_start, length));
        auto * expected = range(source_start + prefix, count);
        if (!producer) expected = ggml_scale_bias(ctx, expected, 2.0f, negative ? 8.0f : 7.0f);
        initialize_tensor(expected_payload, expected);
        initialize_tensor(expected_prefix, range(guard_start, prefix));
        initialize_tensor(expected_suffix, range(guard_start + prefix + count, suffix));
        ggml_build_forward_expand(step, ggml_scale_bias_inplace(ctx, payload, producer ? 1.0f : 2.0f, producer ? 1.0f : 7.0f));
        auto error = [&](ggml_tensor * actual, ggml_tensor * reference) {
            auto * result = ggml_sum(ctx, ggml_sqr(ctx, ggml_sub(ctx, actual, reference)));
            ggml_build_forward_expand(verify, result);
            return result;
        };
        auto * advanced_reference = ggml_scale_bias_inplace(ctx, expected_payload, 1.0f, producer ? 1.0f : 2.0f);
        errors[0] = error(payload, advanced_reference);
        errors[1] = error(ggml_view_1d(ctx, values, prefix, 0), expected_prefix);
        errors[2] = error(ggml_view_1d(ctx, values, suffix, (prefix + count) * sizeof(float)), expected_suffix);
        buffer.reset(ggml_backend_alloc_ctx_tensors(ctx, backend));
        require(bool(buffer), "GPU fixture tensor allocation failed");
        const uintptr_t base = reinterpret_cast<uintptr_t>(ggml_backend_buffer_get_base(payload->buffer));
        const uintptr_t payload_data = reinterpret_cast<uintptr_t>(payload->data);
        const size_t payload_size = ggml_nbytes(payload);
        require(payload_data >= base && payload_data - base <= payload->buffer->size &&
                payload_size <= payload->buffer->size - (payload_data - base), "payload layout exceeds allocation");
        for (auto * error_tensor : errors) {
            require(ggml_nbytes(error_tensor) == sizeof(float), "verification output is not a scalar");
            if (error_tensor->buffer != payload->buffer) continue;
            const uintptr_t error_data = reinterpret_cast<uintptr_t>(error_tensor->data);
            require(error_data >= base && error_data - base <= payload->buffer->size &&
                    sizeof(float) <= payload->buffer->size - (error_data - base), "verification scalar exceeds allocation");
            const size_t error_offset = error_data - base, payload_offset = payload_data - base;
            require(error_offset + sizeof(float) <= payload_offset || payload_offset + payload_size <= error_offset,
                    "verification scalar overlaps prepared payload");
        }
    }
    std::array<float, 3> read_errors() const {
        std::array<float, 3> result{};
        for (size_t i = 0; i < errors.size(); ++i) ggml_backend_tensor_get(errors[i], &result[i], 0, sizeof(result[i]));
        return result;
    }
};

struct sample {
    double producer_submit_us = 0, producer_wait_us = 0, transfer_us = 0;
    double consumer_submit_us = 0, consumer_wait_us = 0, inclusive_us = 0;
    double planning_us = 0, verification_rearm_us = 0, cycle_us = 0;
};
struct case_result {
    size_t bytes = 0;
    bool forward = false, correctness_passed = false, scalar_layout_verified = false;
    unsigned warmup_completed = 0;
    double setup_us = 0;
    std::vector<sample> samples;
    bool failed_verification = false, failed_in_warmup = false;
    unsigned failed_iteration = 0;
    std::array<float, 3> producer_errors{}, consumer_errors{};
};

static bool zeros(const std::array<float, 3> & values) {
    return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value) && value == 0; });
}
static void run_case(const config & c, ggml_backend_t vulkan, ggml_backend_t cuda, case_result & result,
                     clock_type::time_point deadline) {
    auto setup_start = clock_type::now();
    auto producer = result.forward ? vulkan : cuda;
    auto consumer = result.forward ? cuda : vulkan;
    gpu_fixture source(producer, result.bytes, true, false);
    gpu_fixture target(consumer, result.bytes, false, c.negative);
    result.scalar_layout_verified = true;
    compute(producer, source.initialize);
    if (!result.forward) plan(target.payload);
    compute(consumer, target.initialize);
    result.setup_us = us(setup_start, clock_type::now());
    result.samples.reserve(c.iterations);
    for (unsigned i = 0; i < c.warmup + c.iterations; ++i) {
        require(clock_type::now() < deadline, "measurement deadline exceeded between GPU operations");
        auto cycle_begin = clock_type::now();
        if (result.forward) plan(source.payload);
        auto begin = clock_type::now();
        require(ggml_backend_graph_compute_async(producer, source.step) == GGML_STATUS_SUCCESS, "producer submission failed");
        auto producer_submitted = clock_type::now();
        ggml_backend_synchronize(producer);
        auto producer_done = clock_type::now();
        ggml_backend_tensor_copy_async(producer, consumer, source.payload, target.payload);
        ggml_backend_synchronize(consumer);
        auto transfer_done = clock_type::now();
        require(ggml_backend_graph_compute_async(consumer, target.step) == GGML_STATUS_SUCCESS, "consumer submission failed");
        auto consumer_submitted = clock_type::now();
        ggml_backend_synchronize(consumer);
        auto consumer_done = clock_type::now();

        // Verification advances independent GPU references and prepares the next receive.
        compute(producer, source.verify);
        if (!result.forward) plan(target.payload);
        compute(consumer, target.verify);
        result.producer_errors = source.read_errors();
        result.consumer_errors = target.read_errors();
        auto cycle_end = clock_type::now();
        sample current;
        current.producer_submit_us = us(begin, producer_submitted);
        current.producer_wait_us = us(producer_submitted, producer_done);
        current.transfer_us = us(producer_done, transfer_done);
        current.consumer_submit_us = us(transfer_done, consumer_submitted);
        current.consumer_wait_us = us(consumer_submitted, consumer_done);
        current.inclusive_us = us(begin, consumer_done);
        current.planning_us = us(cycle_begin, begin);
        current.verification_rearm_us = us(consumer_done, cycle_end);
        current.cycle_us = us(cycle_begin, cycle_end);
        if (i >= c.warmup) result.samples.push_back(current);
        if (!zeros(result.producer_errors) || !zeros(result.consumer_errors)) {
            result.failed_verification = true;
            result.failed_in_warmup = i < c.warmup;
            result.failed_iteration = i < c.warmup ? i : i - c.warmup;
            return;
        }
        if (i < c.warmup) ++result.warmup_completed;
    }
    result.correctness_passed = true;
}

static std::string quote(const std::string & value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else out << static_cast<char>(c);
    }
    out << '"';
    return out.str();
}
static std::string statistics(std::vector<double> values) {
    if (values.empty()) return "null";
    std::sort(values.begin(), values.end());
    auto percentile = [&](double p) { return values[static_cast<size_t>(std::ceil(p * values.size())) - 1]; };
    const size_t n = values.size();
    const double median = n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) * 0.5;
    std::ostringstream out;
    out << std::setprecision(12) << "{\"count\":" << n << ",\"min_us\":" << values.front() << ",\"median_us\":" << median
        << ",\"p95_us\":" << percentile(.95) << ",\"p99_us\":" << percentile(.99) << ",\"max_us\":" << values.back() << '}';
    return out.str();
}
static const std::pair<const char *, double sample::*> sample_fields[] = {
    {"producer_submit_us", &sample::producer_submit_us}, {"producer_completion_wait_us", &sample::producer_wait_us},
    {"native_tensor_copy_complete_us", &sample::transfer_us}, {"consumer_submit_us", &sample::consumer_submit_us},
    {"consumer_completion_wait_us", &sample::consumer_wait_us}, {"producer_to_consumer_ready_receiver_us", &sample::inclusive_us},
    {"producer_planning_us", &sample::planning_us}, {"gpu_verification_and_receiver_rearm_us", &sample::verification_rearm_us},
    {"full_iteration_cycle_us", &sample::cycle_us},
};
static std::string json(const config & c, const std::vector<case_result> & cases, const std::string & error,
                        bool cleanup, bool correct, bool gate_evaluated, bool gate_passed) {
    std::ostringstream out;
    out << std::setprecision(12) << "{\"schema\":1,\"measurement\":\"producer_submit_to_consumer_gpu_complete_with_already_ready_receiver\""
        << ",\"clock\":\"single_Vulkan_coordinator_steady_clock\",\"cross_host_clock_subtraction\":false,\"round_trip_division\":false"
        << ",\"receiver_ready_before_producer\":true,\"cpu_payload_access\":false,\"gpu_payload_changes_each_iteration\":true"
        << ",\"persistent_payload_allocations_and_mrs\":true,\"persistent_client_graph_objects\":true"
        << ",\"rpc_graph_reuse\":false,\"graph_uid\":0,\"gpu_verification_between_samples\":true"
        << ",\"verification_rearm_and_producer_planning_excluded_from_main_clock\":true"
        << ",\"verification_graphs_can_change_backend_graph_cache_state\":true"
        << ",\"percentiles\":\"p95_p99_nearest_rank_median_midpoint\",\"warmup_requested\":" << c.warmup
        << ",\"iterations_requested\":" << c.iterations << ",\"timeout_seconds\":" << c.timeout_seconds
        << ",\"deadline_scope\":\"checked_between_iterations_existing_backend_timeouts_apply_inside_operations\""
        << ",\"negative_wrong_reference\":" << (c.negative ? "true" : "false")
        << ",\"correctness_passed\":" << (correct ? "true" : "false") << ",\"local_cleanup_completed\":" << (cleanup ? "true" : "false")
        << ",\"remote_cleanup_acknowledged\":false,\"remote_cleanup_evidence\":\"capture_RPC_server_log_separately\""
        << ",\"latency_gate_threshold_us\":" << (c.require_e2e_us ? std::to_string(c.require_e2e_us) : "null")
        << ",\"latency_gate_rule\":\"every_case_observed_max_strictly_less_than_threshold\""
        << ",\"latency_gate_evaluated\":" << (gate_evaluated ? "true" : "false")
        << ",\"latency_gate_passed\":" << (gate_evaluated ? (gate_passed ? "true" : "false") : "null")
        << ",\"error\":" << (error.empty() ? "null" : quote(error)) << ",\"cases\":[";
    for (size_t ci = 0; ci < cases.size(); ++ci) {
        const auto & current = cases[ci];
        if (ci) out << ',';
        out << "{\"direction\":" << quote(current.forward ? "Vulkan_producer_to_CUDA_consumer" : "CUDA_producer_to_Vulkan_consumer")
            << ",\"bytes\":" << current.bytes << ",\"payload_offset_bytes\":" << gpu_fixture::prefix * sizeof(float)
            << ",\"guard_bytes_per_endpoint\":" << (gpu_fixture::prefix + gpu_fixture::suffix) * sizeof(float)
            << ",\"setup_us\":" << current.setup_us << ",\"warmup_completed\":" << current.warmup_completed
            << ",\"measured_iterations\":" << current.samples.size() << ",\"correctness_passed\":" << (current.correctness_passed ? "true" : "false")
            << ",\"reduced_scalars_disjoint_from_payload_verified\":" << (current.scalar_layout_verified ? "true" : "false")
            << ",\"statistics\":{";
        bool first = true;
        for (const auto & field : sample_fields) {
            std::vector<double> values;
            values.reserve(current.samples.size());
            for (const auto & value : current.samples) values.push_back(value.*field.second);
            out << (first ? "" : ",") << quote(field.first) << ':' << statistics(std::move(values));
            first = false;
        }
        out << "},\"samples\":[";
        for (size_t i = 0; i < current.samples.size(); ++i) {
            out << (i ? ",{" : "{");
            first = true;
            for (const auto & field : sample_fields) {
                out << (first ? "" : ",") << quote(field.first) << ':' << current.samples[i].*field.second;
                first = false;
            }
            out << '}';
        }
        out << "],\"verification_failure\":";
        if (!current.failed_verification) out << "null";
        else {
            out << "{\"phase\":" << quote(current.failed_in_warmup ? "warmup" : "measurement") << ",\"iteration\":" << current.failed_iteration;
            for (const auto & item : {std::make_pair("producer_reductions", current.producer_errors), std::make_pair("consumer_reductions", current.consumer_errors)}) {
                out << ',' << quote(item.first) << ":[";
                for (size_t i = 0; i < item.second.size(); ++i) {
                    if (i) out << ',';
                    if (std::isfinite(item.second[i])) out << item.second[i]; else out << "null";
                }
                out << ']';
            }
            out << '}';
        }
        out << '}';
    }
    out << "]}\n";
    return out.str();
}
static void save(int fd, const std::string & text) {
    size_t offset = 0;
    while (offset < text.size()) {
        ssize_t count = ::write(fd, text.data() + offset, text.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "JSON output write failed");
        offset += static_cast<size_t>(count);
    }
    require(fsync(fd) == 0, "JSON output fsync failed");
}

int main(int argc, char ** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") { usage(); return 0; }
    config c;
    int fd = -1;
    std::vector<case_result> results;
    std::string error;
    bool cleanup = false, correct = false, gate_evaluated = false, gate_passed = false;
    int exit_code = 1;
    try {
        c = arguments(argc, argv);
        fd = open(c.output.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        require(fd >= 0, "output must be a new writable private JSON file");
        ggml_backend_load_all();
        auto * device = ggml_backend_dev_by_name("Vulkan0");
        require(device != nullptr, "Vulkan0 is unavailable");
        auto type = ggml_backend_dev_type(device);
        require(type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU, "Vulkan0 is not a hardware GPU");
        backend_ptr vulkan(ggml_backend_dev_init(device, nullptr), ggml_backend_free);
        backend_ptr cuda(ggml_backend_rpc_init(c.rpc.c_str(), 0), ggml_backend_free);
        require(bool(vulkan) && bool(cuda), "Vulkan/RPC initialization failed");
        auto deadline = clock_type::now() + std::chrono::seconds(c.timeout_seconds);
        for (size_t bytes : {size_t(1024), size_t(4096)}) {
            for (bool forward : {true, false}) {
                results.push_back({});
                auto & result = results.back(); result.bytes = bytes; result.forward = forward;
                run_case(c, vulkan.get(), cuda.get(), result, deadline);
                if (!result.correctness_passed) break;
            }
            if (!results.back().correctness_passed) break;
        }
        ggml_backend_synchronize(vulkan.get()); ggml_backend_synchronize(cuda.get());
        vulkan.reset(); cuda.reset(); cleanup = true;
        correct = results.size() == 4 && std::all_of(results.begin(), results.end(), [](const case_result & item) { return item.correctness_passed; });
        exit_code = correct ? 0 : 3;
        if (correct && c.require_e2e_us > 0) {
            gate_evaluated = true; gate_passed = true;
            for (const auto & result : results) for (const auto & value : result.samples) gate_passed = gate_passed && value.inclusive_us < c.require_e2e_us;
            if (!gate_passed) exit_code = 2;
        }
    } catch (const std::exception & exception) {
        error = exception.what(); exit_code = 1;
    }
    if (fd >= 0) {
        try { save(fd, json(c, results, error, cleanup, correct, gate_evaluated, gate_passed)); }
        catch (const std::exception & exception) { std::cerr << "MCDMA_NATIVE_E2E_OUTPUT_FAIL: " << exception.what() << '\n'; exit_code = 1; }
        if (close(fd) != 0) exit_code = 1;
    }
    std::cout << "MCDMA_NATIVE_E2E_RESULT correctness=" << (correct ? "pass" : "fail")
              << " latency_gate=" << (gate_evaluated ? (gate_passed ? "pass" : "fail") : "not_evaluated")
              << " local_cleanup=" << (cleanup ? "pass" : "unconfirmed") << " exit_code=" << exit_code << '\n';
    if (!error.empty()) std::cerr << "MCDMA_NATIVE_E2E_FAIL: " << error << '\n';
    return exit_code;
}
