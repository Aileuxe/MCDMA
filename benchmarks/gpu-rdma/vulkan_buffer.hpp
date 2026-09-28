// Mandatory coherent external-host-memory import, with no allocation fallback.
#include <vulkan/vulkan.h>
#include "mcdma_rpc_vulkan.h"
#include <algorithm>
#include <string>
#include <vector>

constexpr unsigned guard_bytes = 4096, window_bytes = 16384;
constexpr unsigned allocation_bytes = guard_bytes + window_bytes + guard_bytes;
constexpr unsigned slot_bytes = 4096;
enum Pattern { GUARD = 0, FORWARD = 1, REVERSE = 2, POISON = 3 };
static unsigned payload_bytes = 4096;
static unsigned seed = 0;
static uint64_t wr_sequence;

static void vk_check(VkResult result, const char *operation) {
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "VULKAN_ERROR operation=%s result=%d\n", operation, result);
        std::exit(2);
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

static void select_vulkan(unsigned index) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "mcdma-gpu-rdma-check"; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo = &app;
    VK_OK(vkCreateInstance(&create, nullptr, &vulkan.instance));
    uint32_t count = 0; VK_OK(vkEnumeratePhysicalDevices(vulkan.instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    VK_OK(vkEnumeratePhysicalDevices(vulkan.instance, &count, devices.data()));
    if (index >= count) { std::fprintf(stderr, "Vulkan GPU index is unavailable\n"); std::exit(2); }
    vulkan.physical = devices[index];
    vkGetPhysicalDeviceProperties(vulkan.physical, &vulkan.properties);
    std::fprintf(stderr, "GPU_CONFIG backend=vulkan gpu=%u name=%s vendor=0x%x device=0x%x type=%u\n",
                 index, vulkan.properties.deviceName, vulkan.properties.vendorID,
                 vulkan.properties.deviceID, vulkan.properties.deviceType);
    if (vulkan.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
        vulkan.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        std::fprintf(stderr, "Selected Vulkan device must be an integrated or discrete hardware GPU; CPU/software devices are refused\n");
        std::exit(2);
    }
    vkGetPhysicalDeviceMemoryProperties(vulkan.physical, &vulkan.memory_properties);
    VK_OK(vkEnumerateDeviceExtensionProperties(vulkan.physical, nullptr, &count, nullptr));
    std::vector<VkExtensionProperties> extensions(count);
    VK_OK(vkEnumerateDeviceExtensionProperties(vulkan.physical, nullptr, &count, extensions.data()));
    bool host_extension = false, foreign_extension = false;
    for (const auto &extension : extensions) {
        if (!std::strcmp(extension.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) host_extension = true;
        if (!std::strcmp(extension.extensionName, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) foreign_extension = true;
    }
    if (!host_extension || !foreign_extension) {
        std::fprintf(stderr, "Required Vulkan extensions: external_memory_host=%u queue_family_foreign=%u fallback=0\n",
                     host_extension, foreign_extension);
        std::exit(2);
    }
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
        std::fprintf(stderr, "Host allocation import for storage buffers is not supported\n"); std::exit(2);
    }
    vkGetPhysicalDeviceQueueFamilyProperties(vulkan.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(vulkan.physical, &count, families.data());
    for (uint32_t i = 0; i < count; ++i)
        if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) { vulkan.family = i; break; }
    if (vulkan.family == UINT_MAX) { std::fprintf(stderr, "No compute queue\n"); std::exit(2); }
    float priority = 1;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = vulkan.family; queue.queueCount = 1; queue.pQueuePriorities = &priority;
    const char *enabled[] = {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device.queueCreateInfoCount = 1; device.pQueueCreateInfos = &queue;
    device.enabledExtensionCount = 2; device.ppEnabledExtensionNames = enabled;
    VK_OK(vkCreateDevice(vulkan.physical, &device, nullptr, &vulkan.device));
    vkGetDeviceQueue(vulkan.device, vulkan.family, 0, &vulkan.queue);
    vulkan.host_properties = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(vkGetDeviceProcAddr(vulkan.device, "vkGetMemoryHostPointerPropertiesEXT"));
    if (!vulkan.host_properties) { std::fprintf(stderr, "Host import function is unavailable\n"); std::exit(2); }
}

struct Buffer {
    unsigned char *network = nullptr;
    bool foreign_owned = false; // Updated only after a completed release fence.
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
    constexpr VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < vulkan.memory_properties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (vulkan.memory_properties.memoryTypes[i].propertyFlags & required) == required) return i;
    std::fprintf(stderr, "No compatible HOST_VISIBLE|HOST_COHERENT memory type; fallback=0\n"); std::exit(2);
}

struct Parameters { uint32_t operation, offset, length, p0, p1, p2, seed; };

static void dispatch(Buffer &buffer, Parameters parameters) {
    if (!buffer.foreign_owned && parameters.operation != 0) {
        std::fprintf(stderr, "VULKAN_ERROR first dispatch must initialize the entire NIC-shared span\n");
        std::exit(2);
    }
    VK_OK(vkResetFences(vulkan.device, 1, &buffer.fence));
    VK_OK(vkResetCommandBuffer(buffer.command, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_OK(vkBeginCommandBuffer(buffer.command, &begin));
    if (buffer.foreign_owned) {
        // Verbs completion/control ordering precedes this GPU reuse. Ordinary
        // CPU host access does not exempt the separate NIC from ownership.
        VkBufferMemoryBarrier acquire{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        acquire.srcAccessMask = 0;
        acquire.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        acquire.dstQueueFamilyIndex = vulkan.family;
        acquire.buffer = buffer.payload; acquire.offset = 0; acquire.size = allocation_bytes;
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
    vkCmdDispatch(buffer.command, (allocation_bytes / 4 + 63) / 64, 1, 1);
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; after.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;
    vkCmdPipelineBarrier(buffer.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
    VkBufferMemoryBarrier release{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    release.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    release.dstAccessMask = 0;
    release.srcQueueFamilyIndex = vulkan.family;
    release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    release.buffer = buffer.payload; release.offset = 0; release.size = allocation_bytes;
    vkCmdPipelineBarrier(buffer.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 1, &release, 0, nullptr);
    VK_OK(vkEndCommandBuffer(buffer.command));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &buffer.command;
    VK_OK(vkQueueSubmit(vulkan.queue, 1, &submit, buffer.fence));
    VK_OK(vkWaitForFences(vulkan.device, 1, &buffer.fence, VK_TRUE, 10000000000ull));
    buffer.foreign_owned = true;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void gpu_fill(Buffer &buffer, unsigned offset, Pattern pattern) {
    dispatch(buffer, {1, offset, payload_bytes, static_cast<uint32_t>(pattern), 0, 0, seed});
}

static bool gpu_verify(Buffer &buffer, const char *phase, Pattern p0, Pattern p1, Pattern p2) {
    dispatch(buffer, {2, 0, payload_bytes, static_cast<uint32_t>(p0), static_cast<uint32_t>(p1), static_cast<uint32_t>(p2), seed});
    uint32_t errors = *static_cast<volatile uint32_t *>(buffer.error_map);
    std::fprintf(stderr, "GPU_VERIFY phase=%s checked_bytes=%u errors=%u\n", phase, allocation_bytes, errors);
    return errors == 0;
}

static std::vector<uint32_t> shader_words() {
    const char *specified = std::getenv("MCDMA_VULKAN_SHADER");
    char executable[4096];
    ssize_t size = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (size <= 0) { std::perror("locate executable"); std::exit(2); }
    executable[size] = 0;
    char *slash = std::strrchr(executable, '/'); if (slash) slash[1] = 0;
    std::string filename = specified ? specified : std::string(executable) + "gpu_verbs_shader.spv";
    FILE *file = std::fopen(filename.c_str(), "rb"); if (!file) { std::perror("open compute shader"); std::exit(2); }
    if (std::fseek(file, 0, SEEK_END)) { std::perror("seek shader"); std::exit(2); }
    long bytes = std::ftell(file);
    if (bytes <= 0 || bytes % 4 || bytes > 1048576) { std::fprintf(stderr, "Invalid shader size\n"); std::exit(2); }
    std::rewind(file); std::vector<uint32_t> words(bytes / 4);
    if (std::fread(words.data(), 1, bytes, file) != static_cast<size_t>(bytes)) { std::fprintf(stderr, "Cannot read shader\n"); std::exit(2); }
    std::fclose(file); return words;
}

static void allocate_buffer(Buffer &buffer, bool device) {
    if (device) { std::fprintf(stderr, "Vulkan device-allocation MR probe is unavailable; no host fallback\n"); std::exit(3); }
    long page = sysconf(_SC_PAGESIZE);
    VkDeviceSize alignment = std::max<VkDeviceSize>(page > 0 ? static_cast<VkDeviceSize>(page) : 0, vulkan.import_alignment);
    if (page <= 0 || !alignment || (alignment & (alignment - 1)) || allocation_bytes % alignment ||
        posix_memalign(reinterpret_cast<void **>(&buffer.network), static_cast<size_t>(alignment), allocation_bytes)) {
        std::fprintf(stderr, "VULKAN_WRAPPER_ERROR cannot allocate exact aligned span; fallback=0\n"); std::exit(2);
    }
    if (mcdma_rpc_vulkan_abi() != MCDMA_RPC_GPU_ABI) {
        std::fprintf(stderr, "VULKAN_WRAPPER_ERROR ABI mismatch\n"); std::exit(2);
    }
    mcdma_rpc_gpu_error error{};
    int status = mcdma_rpc_vulkan_wrap(vulkan.physical, vulkan.device, buffer.network, allocation_bytes,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &buffer.wrapper, &error);
    if (status) {
        std::fprintf(stderr, "VULKAN_WRAPPER_ERROR status=%d backend=%d reason=%s\n", status, error.backend_code, error.message);
        std::exit(2);
    }
    buffer.payload = mcdma_rpc_vulkan_buffer(buffer.wrapper);
    if (!buffer.payload || mcdma_rpc_vulkan_size(buffer.wrapper) != allocation_bytes) {
        std::fprintf(stderr, "VULKAN_WRAPPER_ERROR invalid imported span\n"); std::exit(2);
    }
    std::fprintf(stderr, "GPU_WRAPPER backend=vulkan abi=%u bytes=%u payload_cpu_copy_bytes=0\n",
                 mcdma_rpc_vulkan_abi(), allocation_bytes);
    std::fprintf(stderr, "VULKAN_ALLOCATION mode=external-host-import host_visible=1 host_coherent=1 registered_bytes=%u backing_bytes=%u fallback=0\n",
                 allocation_bytes, allocation_bytes);

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
    VkDescriptorBufferInfo payload{buffer.payload, 0, allocation_bytes}, errors{buffer.errors, 0, sizeof(uint32_t)};
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
    dispatch(buffer, {0, 0, payload_bytes, 0, 0, 0, seed});
}

static void free_buffer(Buffer &buffer) {
    VK_OK(vkDeviceWaitIdle(vulkan.device));
    vkDestroyFence(vulkan.device, buffer.fence, nullptr);
    vkDestroyCommandPool(vulkan.device, buffer.command_pool, nullptr);
    vkDestroyPipeline(vulkan.device, buffer.pipeline, nullptr);
    vkDestroyPipelineLayout(vulkan.device, buffer.pipeline_layout, nullptr);
    vkDestroyDescriptorPool(vulkan.device, buffer.descriptor_pool, nullptr);
    vkDestroyDescriptorSetLayout(vulkan.device, buffer.descriptors, nullptr);
    vkUnmapMemory(vulkan.device, buffer.error_memory);
    vkDestroyBuffer(vulkan.device, buffer.errors, nullptr); vkFreeMemory(vulkan.device, buffer.error_memory, nullptr);
    mcdma_rpc_gpu_error error{};
    int status = mcdma_rpc_vulkan_release(&buffer.wrapper, &error);
    if (status) {
        std::fprintf(stderr, "VULKAN_WRAPPER_ERROR release_status=%d backend=%d reason=%s\n", status, error.backend_code, error.message);
        std::exit(2);
    }
    std::fprintf(stderr, "GPU_WRAPPER_RELEASE backend=vulkan cleanup=ok\n");
    std::free(buffer.network);
    vkDestroyDevice(vulkan.device, nullptr); vkDestroyInstance(vulkan.instance, nullptr);
}
