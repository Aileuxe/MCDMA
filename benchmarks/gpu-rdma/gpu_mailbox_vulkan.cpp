// SPDX-License-Identifier: Apache-2.0
// Compute resources adapted from the neighboring Vulkan verbs validator.
#include "mailbox_support.hpp"
#include <vulkan/vulkan.h>
#include "mcdma_rpc_vulkan.h"
#include <algorithm>
#include <string>
#include <vector>

static void vk_check(VkResult result, const char *operation) {
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "VULKAN_ERROR operation=%s result=%d\n", operation, result);
        throw std::runtime_error("Vulkan operation failed");
    }
}
#define VK_OK(call) vk_check((call), #call)

struct VulkanState {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = UINT_MAX;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkDeviceSize import_alignment = 0;
    bool dedicated_required = false;
    PFN_vkGetMemoryHostPointerPropertiesEXT host_properties = nullptr;
};
static VulkanState vulkan;
#define MCDMA_MAILBOX_EXPORT 1
static bool export_mode = false;
static mcdma_rpc_vulkan *mailbox_owner = nullptr;
static unsigned char *mailbox_owner_pointer = nullptr;
static int mailbox_owner_fd = -1;
static void gpu_set_export_mode(bool enabled) { export_mode = enabled; }

static void select_vulkan(unsigned index) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "mcdma-gpu-rdma-check"; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo = &app;
    VK_OK(vkCreateInstance(&create, nullptr, &vulkan.instance));
    uint32_t count = 0; VK_OK(vkEnumeratePhysicalDevices(vulkan.instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    VK_OK(vkEnumeratePhysicalDevices(vulkan.instance, &count, devices.data()));
    if (index >= count) { std::fprintf(stderr, "Vulkan GPU index is unavailable\n"); throw std::runtime_error("Vulkan mailbox setup or release failed"); }
    vulkan.physical = devices[index];
    vkGetPhysicalDeviceProperties(vulkan.physical, &vulkan.properties);
    std::fprintf(stderr, "GPU_CONFIG backend=vulkan gpu=%u name=%s vendor=0x%x device=0x%x type=%u\n",
                 index, vulkan.properties.deviceName, vulkan.properties.vendorID,
                 vulkan.properties.deviceID, vulkan.properties.deviceType);
    if (vulkan.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
        vulkan.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        std::fprintf(stderr, "Selected Vulkan device must be an integrated or discrete hardware GPU; CPU/software devices are refused\n");
        throw std::runtime_error("Vulkan mailbox setup or release failed");
    }
    vkGetPhysicalDeviceMemoryProperties(vulkan.physical, &vulkan.memory_properties);
    VK_OK(vkEnumerateDeviceExtensionProperties(vulkan.physical, nullptr, &count, nullptr));
    std::vector<VkExtensionProperties> extensions(count);
    VK_OK(vkEnumerateDeviceExtensionProperties(vulkan.physical, nullptr, &count, extensions.data()));
    std::vector<const char *> enabled = export_mode
        ? std::vector<const char *>{VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                                    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME}
        : std::vector<const char *>{VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
    for (const char *name : enabled) {
        bool present = false;
        for (const auto &extension : extensions) if (!std::strcmp(extension.extensionName, name)) present = true;
        mailbox_require(present, "required Vulkan external-memory extension is unavailable");
    }
    if (!export_mode) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT host{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; properties.pNext = &host;
        vkGetPhysicalDeviceProperties2(vulkan.physical, &properties);
        vulkan.import_alignment = host.minImportedHostPointerAlignment;
        VkPhysicalDeviceExternalBufferInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkExternalBufferProperties external{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        vkGetPhysicalDeviceExternalBufferProperties(vulkan.physical, &info, &external);
        auto features = external.externalMemoryProperties.externalMemoryFeatures;
        vulkan.dedicated_required = !!(features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT);
        std::fprintf(stderr, "VULKAN_CAPABILITIES external_memory_host=1 min_import_alignment=%llu importable=%u dedicated_only=%u fallback=0\n",
                     static_cast<unsigned long long>(vulkan.import_alignment),
                     !!(features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT), !!(features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT));
        if (!(features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
            std::fprintf(stderr, "Host allocation import for storage buffers is not supported\n"); throw std::runtime_error("Vulkan mailbox setup or release failed");
        }
    }
    vkGetPhysicalDeviceQueueFamilyProperties(vulkan.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(vulkan.physical, &count, families.data());
    for (uint32_t i = 0; i < count; ++i)
        if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) { vulkan.family = i; break; }
    if (vulkan.family == UINT_MAX) { std::fprintf(stderr, "No compute queue\n"); throw std::runtime_error("Vulkan mailbox setup or release failed"); }
    float priority = 1;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = vulkan.family; queue.queueCount = 1; queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device.queueCreateInfoCount = 1; device.pQueueCreateInfos = &queue;
    device.enabledExtensionCount = static_cast<uint32_t>(enabled.size()); device.ppEnabledExtensionNames = enabled.data();
    VK_OK(vkCreateDevice(vulkan.physical, &device, nullptr, &vulkan.device));
    vkGetDeviceQueue(vulkan.device, vulkan.family, 0, &vulkan.queue);
    vulkan.host_properties = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(vkGetDeviceProcAddr(vulkan.device, "vkGetMemoryHostPointerPropertiesEXT"));
    if (!export_mode && !vulkan.host_properties) { std::fprintf(stderr, "Host import function is unavailable\n"); throw std::runtime_error("Vulkan mailbox setup or release failed"); }
}

struct Buffer {
    unsigned char *network = nullptr;
    unsigned bytes = 0;
    VkDeviceSize offset = 0;
    bool foreign_owned = false; // Becomes true only after a completed release fence.
    VkBuffer payload = VK_NULL_HANDLE, errors = VK_NULL_HANDLE;
    VkDeviceMemory error_memory = VK_NULL_HANDLE;
    mcdma_rpc_vulkan *wrapper = nullptr;
    void *error_map = nullptr;
    VkDescriptorSetLayout descriptors = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
};

static uint32_t coherent_memory(uint32_t bits) {
    constexpr VkMemoryPropertyFlags standard = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    constexpr VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < vulkan.memory_properties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (vulkan.memory_properties.memoryTypes[i].propertyFlags & required) == required &&
            !(vulkan.memory_properties.memoryTypes[i].propertyFlags & ~standard)) return i;
    std::fprintf(stderr, "No compatible HOST_VISIBLE|HOST_COHERENT memory type; fallback=0\n"); throw std::runtime_error("Vulkan mailbox setup or release failed");
}

struct Parameters { uint32_t operation, bytes, length, sequence, kind; };

static void dispatch(Buffer &buffer, Parameters parameters) {
    mailbox_require(buffer.foreign_owned || parameters.operation == 1,
                    "a NIC-shared payload view must begin with a full GPU overwrite");
    VK_OK(vkResetFences(vulkan.device, 1, &buffer.fence));
    VK_OK(vkResetCommandBuffer(buffer.command, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_OK(vkBeginCommandBuffer(buffer.command, &begin));
    if (buffer.foreign_owned) {
        // The caller has already observed NIC completion before GPU reuse.
        // Acquire exactly this descriptor range, excluding both control pages.
        VkBufferMemoryBarrier acquire{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        acquire.srcAccessMask = 0;
        acquire.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        acquire.dstQueueFamilyIndex = vulkan.family;
        acquire.buffer = buffer.payload; acquire.offset = buffer.offset; acquire.size = buffer.bytes;
        vkCmdPipelineBarrier(buffer.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 1, &acquire, 0, nullptr);
    }
    if (parameters.operation == 2) vkCmdFillBuffer(buffer.command, buffer.errors, 0, sizeof(uint32_t), 0);
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    before.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(buffer.command, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(buffer.command, VK_PIPELINE_BIND_POINT_COMPUTE, buffer.pipeline);
    vkCmdBindDescriptorSets(buffer.command, VK_PIPELINE_BIND_POINT_COMPUTE, buffer.pipeline_layout, 0, 1, &buffer.descriptor_set, 0, nullptr);
    vkCmdPushConstants(buffer.command, buffer.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters), &parameters);
    vkCmdDispatch(buffer.command, (buffer.bytes / 4 + 63) / 64, 1, 1);
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; after.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;
    vkCmdPipelineBarrier(buffer.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
    {
        // The same exact range is acquired on its next GPU dispatch. The NIC
        // belongs to a foreign device/driver, not Vulkan's EXTERNAL family.
        VkBufferMemoryBarrier release{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        release.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        release.dstAccessMask = 0;
        release.srcQueueFamilyIndex = vulkan.family;
        release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        release.buffer = buffer.payload; release.offset = buffer.offset; release.size = buffer.bytes;
        vkCmdPipelineBarrier(buffer.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 1, &release, 0, nullptr);
    }
    VK_OK(vkEndCommandBuffer(buffer.command));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &buffer.command;
    VK_OK(vkQueueSubmit(vulkan.queue, 1, &submit, buffer.fence));
    VK_OK(vkWaitForFences(vulkan.device, 1, &buffer.fence, VK_TRUE, 10000000000ull));
    buffer.foreign_owned = true;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

using GPUBuffer = Buffer;
static const char *selected_shader;
static void gpu_init(unsigned index, const char *shader) {
    selected_shader = shader;
    select_vulkan(index);
    mailbox_require(mcdma_rpc_vulkan_abi() == MCDMA_RPC_GPU_ABI, "Vulkan wrapper ABI mismatch");
    std::printf("GPU_MAILBOX_BACKEND backend=vulkan gpu=%u name=%s wrapper_abi=%u\n",
                index, vulkan.properties.deviceName, mcdma_rpc_vulkan_abi());
    std::puts("GPU_MAILBOX_OWNERSHIP mode=foreign-queue-family per_payload_view=1 control_pages_excluded=1");
}
static void gpu_owned_allocate(size_t bytes) {
    mcdma_rpc_gpu_error error{};
    int status = mcdma_rpc_vulkan_allocate_export(vulkan.physical, vulkan.device, bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &mailbox_owner, &error);
    if (status) throw std::runtime_error(error.message);
    mailbox_owner_pointer = static_cast<unsigned char *>(mcdma_rpc_vulkan_host_pointer(mailbox_owner));
    mailbox_require(mailbox_owner_pointer && mcdma_rpc_vulkan_size(mailbox_owner) == bytes, "invalid owned GPU mailbox span");
    status = mcdma_rpc_vulkan_export_fd(mailbox_owner, &mailbox_owner_fd, &error);
    if (status) throw std::runtime_error(error.message);
    std::printf("GPU_MAILBOX_OWNED backend=vulkan bytes=%zu dma_buf=1 payload_cpu_copy_bytes=0\n", bytes);
}
static unsigned char *gpu_owned_pointer() { return mailbox_owner_pointer; }
static int gpu_owned_fd() { return mailbox_owner_fd; }
static void gpu_owned_release() {
    mcdma_rpc_gpu_error error{};
    if (mcdma_rpc_vulkan_release(&mailbox_owner, &error)) throw std::runtime_error(error.message);
    if (mailbox_owner_fd >= 0) close(mailbox_owner_fd);
    mailbox_owner_fd = -1; mailbox_owner_pointer = nullptr;
    std::puts("GPU_MAILBOX_OWNED_RELEASE cleanup=ok");
}
static void gpu_fill(Buffer &buffer, unsigned length, unsigned sequence, unsigned kind) {
    dispatch(buffer, {1, buffer.bytes, length, sequence, kind});
}
static void gpu_verify(Buffer &buffer, unsigned length, unsigned sequence, unsigned kind, const char *phase) {
    dispatch(buffer, {2, buffer.bytes, length, sequence, kind});
    uint32_t errors = *static_cast<volatile uint32_t *>(buffer.error_map);
    std::printf("GPU_MAILBOX_VERIFY phase=%s seq=%u payload_bytes=%u checked_bytes=%u errors=%u\n",
                phase, sequence, length, buffer.bytes, errors);
    mailbox_require(errors == 0, "GPU mailbox verification failed");
}

static std::vector<uint32_t> shader_words() {
    const char *specified = selected_shader;
    char executable[4096];
    ssize_t size = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (size <= 0) { std::perror("locate executable"); throw std::runtime_error("Vulkan mailbox setup or release failed"); }
    executable[size] = 0;
    char *slash = std::strrchr(executable, '/'); if (slash) slash[1] = 0;
    std::string filename = specified ? specified : std::string(executable) + "gpu_mailbox_shader.spv";
    FILE *file = std::fopen(filename.c_str(), "rb"); if (!file) { std::perror("open compute shader"); throw std::runtime_error("Vulkan mailbox setup or release failed"); }
    if (std::fseek(file, 0, SEEK_END)) { std::perror("seek shader"); throw std::runtime_error("Vulkan mailbox setup or release failed"); }
    long bytes = std::ftell(file);
    if (bytes <= 0 || bytes % 4 || bytes > 1048576) { std::fprintf(stderr, "Invalid shader size\n"); throw std::runtime_error("Vulkan mailbox setup or release failed"); }
    std::rewind(file); std::vector<uint32_t> words(bytes / 4);
    if (std::fread(words.data(), 1, bytes, file) != static_cast<size_t>(bytes)) { std::fprintf(stderr, "Cannot read shader\n"); throw std::runtime_error("Vulkan mailbox setup or release failed"); }
    std::fclose(file); return words;
}

static void gpu_wrap(Buffer &buffer, unsigned char *memory, unsigned bytes) {
    buffer.network = memory;
    buffer.bytes = bytes;
    if (mailbox_owner) {
        uintptr_t begin = reinterpret_cast<uintptr_t>(mailbox_owner_pointer);
        uintptr_t pointer = reinterpret_cast<uintptr_t>(memory);
        size_t size = mcdma_rpc_vulkan_size(mailbox_owner);
        mailbox_require(pointer >= begin && pointer - begin <= size && bytes <= size - (pointer - begin), "GPU descriptor view is outside owned mailbox");
        buffer.offset = pointer - begin;
        mailbox_require(!(buffer.offset % vulkan.properties.limits.minStorageBufferOffsetAlignment), "GPU descriptor offset alignment mismatch");
        buffer.payload = mcdma_rpc_vulkan_buffer(mailbox_owner);
        std::printf("GPU_MAILBOX_VIEW backend=vulkan bytes=%u owned_allocation=1 payload_cpu_copy_bytes=0\n", bytes);
    } else {
        if (mcdma_rpc_vulkan_abi() != MCDMA_RPC_GPU_ABI) {
            std::fprintf(stderr, "VULKAN_WRAPPER_ERROR ABI mismatch\n"); throw std::runtime_error("Vulkan mailbox setup or release failed");
        }
        mcdma_rpc_gpu_error error{};
        int status = mcdma_rpc_vulkan_wrap(vulkan.physical, vulkan.device, buffer.network, buffer.bytes,
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &buffer.wrapper, &error);
        if (status) {
            std::fprintf(stderr, "VULKAN_WRAPPER_ERROR status=%d backend=%d reason=%s\n", status, error.backend_code, error.message);
            throw std::runtime_error("Vulkan mailbox setup or release failed");
        }
        buffer.payload = mcdma_rpc_vulkan_buffer(buffer.wrapper);
        if (!buffer.payload || mcdma_rpc_vulkan_size(buffer.wrapper) != buffer.bytes) {
            std::fprintf(stderr, "VULKAN_WRAPPER_ERROR invalid imported span\n"); throw std::runtime_error("Vulkan mailbox setup or release failed");
        }
        std::fprintf(stderr, "GPU_WRAPPER backend=vulkan abi=%u bytes=%u payload_cpu_copy_bytes=0\n",
                     mcdma_rpc_vulkan_abi(), buffer.bytes);
        std::fprintf(stderr, "GPU_MAILBOX_VULKAN_ALLOCATION mode=external-host-import host_visible=1 host_coherent=1 imported_bytes=%u backing_bytes=%u fallback=0\n",
                     buffer.bytes, buffer.bytes);

    }

    VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkMemoryRequirements requirements{};
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    create.pNext = nullptr; create.size = sizeof(uint32_t); create.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VK_OK(vkCreateBuffer(vulkan.device, &create, nullptr, &buffer.errors));
    vkGetBufferMemoryRequirements(vulkan.device, buffer.errors, &requirements);
    allocate.pNext = nullptr; allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = coherent_memory(requirements.memoryTypeBits);
    VK_OK(vkAllocateMemory(vulkan.device, &allocate, nullptr, &buffer.error_memory));
    VK_OK(vkBindBufferMemory(vulkan.device, buffer.errors, buffer.error_memory, 0));
    VK_OK(vkMapMemory(vulkan.device, buffer.error_memory, 0, sizeof(uint32_t), 0, &buffer.error_map));

    VkDescriptorSetLayoutBinding bindings[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding = i; bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1; bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; layout.bindingCount = 2; layout.pBindings = bindings;
    VK_OK(vkCreateDescriptorSetLayout(vulkan.device, &layout, nullptr, &buffer.descriptors));
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets = 1; pool.poolSizeCount = 1; pool.pPoolSizes = &pool_size;
    VK_OK(vkCreateDescriptorPool(vulkan.device, &pool, nullptr, &buffer.descriptor_pool));
    VkDescriptorSetAllocateInfo sets{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    sets.descriptorPool = buffer.descriptor_pool; sets.descriptorSetCount = 1; sets.pSetLayouts = &buffer.descriptors;
    VK_OK(vkAllocateDescriptorSets(vulkan.device, &sets, &buffer.descriptor_set));
    VkDescriptorBufferInfo payload{buffer.payload, buffer.offset, buffer.bytes}, errors{buffer.errors, 0, sizeof(uint32_t)};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = buffer.descriptor_set;
        writes[i].dstBinding = i; writes[i].descriptorCount = 1; writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = i ? &errors : &payload;
    }
    vkUpdateDescriptorSets(vulkan.device, 2, writes, 0, nullptr);
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Parameters)};
    VkPipelineLayoutCreateInfo pipeline_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout.setLayoutCount = 1; pipeline_layout.pSetLayouts = &buffer.descriptors;
    pipeline_layout.pushConstantRangeCount = 1; pipeline_layout.pPushConstantRanges = &range;
    VK_OK(vkCreatePipelineLayout(vulkan.device, &pipeline_layout, nullptr, &buffer.pipeline_layout));
    std::vector<uint32_t> words = shader_words();
    VkShaderModuleCreateInfo shader_create{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; shader_create.codeSize = words.size() * 4; shader_create.pCode = words.data();
    VkShaderModule shader; VK_OK(vkCreateShaderModule(vulkan.device, &shader_create, nullptr, &shader));
    VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    compute.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    compute.stage.module = shader; compute.stage.pName = "main"; compute.layout = buffer.pipeline_layout;
    VK_OK(vkCreateComputePipelines(vulkan.device, VK_NULL_HANDLE, 1, &compute, nullptr, &buffer.pipeline));
    vkDestroyShaderModule(vulkan.device, shader, nullptr);
    VkCommandPoolCreateInfo commands{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; commands.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commands.queueFamilyIndex = vulkan.family; VK_OK(vkCreateCommandPool(vulkan.device, &commands, nullptr, &buffer.command_pool));
    VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command.commandPool = buffer.command_pool; command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; command.commandBufferCount = 1;
    VK_OK(vkAllocateCommandBuffers(vulkan.device, &command, &buffer.command));
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VK_OK(vkCreateFence(vulkan.device, &fence, nullptr, &buffer.fence));
    if (!mailbox_owner) std::printf("GPU_MAILBOX_IMPORT backend=vulkan bytes=%u same_mailbox_pages=1 payload_cpu_copy_bytes=0\n", buffer.bytes);
}

static void gpu_release(Buffer &buffer) {
    VK_OK(vkDeviceWaitIdle(vulkan.device));
    vkDestroyFence(vulkan.device, buffer.fence, nullptr);
    vkDestroyCommandPool(vulkan.device, buffer.command_pool, nullptr);
    vkDestroyPipeline(vulkan.device, buffer.pipeline, nullptr);
    vkDestroyPipelineLayout(vulkan.device, buffer.pipeline_layout, nullptr);
    vkDestroyDescriptorPool(vulkan.device, buffer.descriptor_pool, nullptr);
    vkDestroyDescriptorSetLayout(vulkan.device, buffer.descriptors, nullptr);
    vkUnmapMemory(vulkan.device, buffer.error_memory);
    vkDestroyBuffer(vulkan.device, buffer.errors, nullptr); vkFreeMemory(vulkan.device, buffer.error_memory, nullptr);
    if (buffer.wrapper) {
        mcdma_rpc_gpu_error error{};
        int status = mcdma_rpc_vulkan_release(&buffer.wrapper, &error);
        if (status) {
            std::fprintf(stderr, "VULKAN_WRAPPER_ERROR release_status=%d backend=%d reason=%s\n", status, error.backend_code, error.message);
            throw std::runtime_error("Vulkan mailbox setup or release failed");
        }
        std::fprintf(stderr, "GPU_WRAPPER_RELEASE backend=vulkan cleanup=ok\n");
    } else std::puts("GPU_MAILBOX_VIEW_RELEASE backend=vulkan cleanup=ok");
}
static void gpu_shutdown() {
    vkDestroyDevice(vulkan.device, nullptr); vkDestroyInstance(vulkan.instance, nullptr);
}

#include "mailbox_app.hpp"
