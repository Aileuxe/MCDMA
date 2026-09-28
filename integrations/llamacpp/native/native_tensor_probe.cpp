#include "ggml.h"
#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml-backend-impl.h"
#include "ggml-rpc.h"

#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

static void require(bool ok, const char * message) { if (!ok) throw std::runtime_error(message); }
using backend_ptr = std::unique_ptr<ggml_backend, decltype(&ggml_backend_free)>;
using context_ptr = std::unique_ptr<ggml_context, decltype(&ggml_free)>;
using buffer_ptr = std::unique_ptr<ggml_backend_buffer, decltype(&ggml_backend_buffer_free)>;

struct tensor_set {
    context_ptr context{nullptr, ggml_free};
    buffer_ptr buffer{nullptr, ggml_backend_buffer_free};
    tensor_set() {
        ggml_init_params params{};
        params.mem_size = 96 * ggml_tensor_overhead() + 6 * ggml_graph_overhead_custom(64, false);
        params.no_alloc = true;
        context.reset(ggml_init(params));
        require(bool(context), "tensor metadata allocation failed");
    }
    ggml_tensor * tensor(size_t length, const char * name) {
        auto * result = ggml_new_tensor_1d(context.get(), GGML_TYPE_F32, (int64_t) length);
        ggml_set_name(result, name);
        return result;
    }
    ggml_cgraph * graph(std::initializer_list<ggml_tensor *> roots) {
        auto * result = ggml_new_graph_custom(context.get(), 64, false);
        for (auto * root : roots) ggml_build_forward_expand(result, root);
        return result;
    }
    ggml_tensor * error(ggml_tensor * actual, ggml_tensor * expected) {
        return ggml_sum(context.get(), ggml_sqr(context.get(), ggml_sub(context.get(), actual, expected)));
    }
    void allocate(ggml_backend_t backend) {
        buffer.reset(ggml_backend_alloc_ctx_tensors(context.get(), backend));
        require(bool(buffer), "GPU tensor allocation failed");
    }
};

static void upload(ggml_tensor * tensor, const std::vector<float> & values) {
    require(ggml_nbytes(tensor) == values.size() * sizeof(float), "seed tensor length mismatch");
    ggml_backend_tensor_set(tensor, values.data(), 0, values.size() * sizeof(float));
}

static void compute(ggml_backend_t backend, ggml_cgraph * graph) {
    require(ggml_backend_graph_compute(backend, graph) == GGML_STATUS_SUCCESS, "GPU graph failed");
}

static void plan(ggml_tensor * tensor) {
    auto buffer = tensor->buffer;
    require(buffer && buffer->iface.dma_plan, "Vulkan producer planning hook unavailable");
    uintptr_t base = (uintptr_t) ggml_backend_buffer_get_base(buffer);
    uintptr_t data = (uintptr_t) tensor->data;
    size_t size = ggml_nbytes(tensor);
    require(data >= base && data - base <= buffer->size && size <= buffer->size - (data - base),
            "planned tensor range is outside allocation");
    require(buffer->iface.dma_plan(buffer, data - base, size), "Vulkan producer planning failed");
}

static float read_error(ggml_tensor * tensor) {
    require(ggml_nbytes(tensor) == sizeof(float), "GPU reduction is not scalar");
    float result = -1;
    ggml_backend_tensor_get(tensor, &result, 0, sizeof(result));
    require(std::isfinite(result), "GPU validation returned nonfinite error");
    return result;
}

static void run(ggml_backend_t vulkan, ggml_backend_t cuda, size_t bytes, unsigned seed, bool corrupt) {
    const size_t prefix = 80, suffix = 96;
    require(bytes && bytes % sizeof(float) == 0, "payload size must be float aligned");
    const size_t count = bytes / sizeof(float), length = prefix + count + suffix;
    std::vector<float> seed_values(length, -17), source_expected(length, -34);
    std::vector<float> target_seed(length, -13), target_expected_seed(length, -13);
    std::vector<float> remote_seed(length, -7), remote_expected(length, -37);
    for (size_t i = 0; i < count; ++i) {
        float value = (float) ((i * 17 + seed) % 1024);
        seed_values[prefix + i] = value;
        source_expected[prefix + i] = value * 2;
        target_expected_seed[prefix + i] = value * 3 + 2.5f;
        remote_expected[prefix + i] = value * 6 + 5;
    }
    if (corrupt) target_expected_seed[prefix + count / 2] += 1;

    tensor_set local;
    auto * source = local.tensor(length, "mcdma-source");
    auto * source_reference = local.tensor(length, "mcdma-source-reference");
    auto * target = local.tensor(length, "mcdma-target");
    auto * expected = local.tensor(length, "mcdma-target-reference");
    auto * source_view = ggml_view_1d(local.context.get(), source, count, prefix * sizeof(float));
    auto * target_view = ggml_view_1d(local.context.get(), target, count, prefix * sizeof(float));
    ggml_set_name(source_view, "mcdma-source-span");
    ggml_set_name(target_view, "mcdma-target-span");
    auto * local_init = local.graph({
        ggml_scale_inplace(local.context.get(), source, 2),
        ggml_scale_inplace(local.context.get(), target, 2),
        ggml_scale_inplace(local.context.get(), expected, 2),
    });
    auto * source_error = local.error(source, source_reference);
    auto * target_error = local.error(target, expected);
    auto * local_check = local.graph({source_error, target_error});
    local.allocate(vulkan);

    tensor_set remote;
    auto * received = remote.tensor(length, "mcdma-cuda-received");
    auto * remote_reference = remote.tensor(length, "mcdma-cuda-reference");
    auto * remote_view = ggml_view_1d(remote.context.get(), received, count, prefix * sizeof(float));
    ggml_set_name(remote_view, "mcdma-cuda-span");
    auto * remote_init = remote.graph({ggml_scale_inplace(remote.context.get(), received, 2)});
    auto * remote_transform = remote.graph({ggml_scale_bias_inplace(remote.context.get(), received, 3, 5)});
    auto * remote_error = remote.error(received, remote_reference);
    auto * remote_check = remote.graph({remote_error});
    remote.allocate(cuda);

    // CPU initializes seeds and independent expectations; transferred values are GPU results.
    upload(source, seed_values); upload(source_reference, source_expected);
    upload(target, target_seed); upload(expected, target_expected_seed);
    upload(received, remote_seed); upload(remote_reference, remote_expected);
    plan(source_view);
    plan(target_view);
    compute(vulkan, local_init);
    compute(cuda, remote_init);
    auto forward_start = std::chrono::steady_clock::now();
    ggml_backend_tensor_copy_async(vulkan, cuda, source_view, remote_view);
    ggml_backend_synchronize(cuda);
    double forward_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - forward_start).count();
    compute(cuda, remote_transform);
    compute(cuda, remote_check);
    auto reverse_start = std::chrono::steady_clock::now();
    ggml_backend_tensor_copy_async(cuda, vulkan, remote_view, target_view);
    ggml_backend_synchronize(vulkan);
    double reverse_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - reverse_start).count();
    compute(vulkan, local_check);

    float cuda_error = read_error(remote_error);
    float original_error = read_error(source_error);
    float returned_error = read_error(target_error);
    require(cuda_error == 0 && original_error == 0 && returned_error == 0,
            "GPU payload/guard verification failed");
    std::cout << "MCDMA_NATIVE_TENSOR_CASE_PASS bytes=" << bytes << " seed=" << seed
              << " guard_bytes=" << (prefix + suffix) * sizeof(float)
              << " cuda_error=" << cuda_error << " source_error=" << original_error
              << " returned_error=" << returned_error
              << " tensor_copy_forward_us=" << forward_us << " tensor_copy_reverse_us=" << reverse_us << '\n';
}

int main(int argc, char ** argv) {
    try {
        require(argc == 2 || (argc == 3 && std::string(argv[2]) == "--negative"),
                "usage: mcdma-native-tensor-probe RPC_ENDPOINT [--negative]");
        const char * enabled = std::getenv("GGML_MCDMA");
        require(enabled && std::string(enabled) == "1", "GGML_MCDMA=1 is required");
        ggml_backend_load_all();
        auto device = ggml_backend_dev_by_name("Vulkan0");
        require(device != nullptr, "Vulkan0 unavailable");
        auto type = ggml_backend_dev_type(device);
        require(type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU,
                "Vulkan0 is not a GPU");
        backend_ptr vulkan(ggml_backend_dev_init(device, nullptr), ggml_backend_free);
        backend_ptr cuda(ggml_backend_rpc_init(argv[1], 0), ggml_backend_free);
        require(bool(vulkan) && bool(cuda), "Vulkan/RPC backend initialization failed");
        bool negative = argc == 3;
        for (size_t bytes : {size_t(1024), size_t(4096), size_t(1024 * 1024)}) {
            for (unsigned repetition = 0; repetition < 2; ++repetition) {
                run(vulkan.get(), cuda.get(), bytes, 17 + repetition * 6, negative);
            }
        }
        ggml_backend_synchronize(vulkan.get()); ggml_backend_synchronize(cuda.get());
        std::cout << "MCDMA_NATIVE_TENSOR_PASS cases=6 strict_native_path=1\n";
        return 0;
    } catch (const std::exception & error) {
        std::cerr << "MCDMA_NATIVE_TENSOR_FAIL: " << error.what() << '\n';
        return 1;
    }
}
