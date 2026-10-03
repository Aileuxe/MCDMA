# Optional GPU memory for RPC mailboxes

These Linux libraries let an application use its existing `mcdma-rpcd` mailbox
as CUDA mapped host memory or a Vulkan storage buffer. The wrapping APIs import
the caller's allocation; they never replace it, copy payload bytes or free it.
Vulkan also has an explicit allocation API for a mapped buffer exportable as
DMA_BUF when importing a preexisting mailbox is unsupported. None of these
operations opens a verbs context. The daemon remains responsible for RDMA
registration and queue pairs.

The ordinary RPC library and macOS Metal helper do not depend on either library.
Applications opt in explicitly and must handle errors; there is no implicit
fallback to a payload copy. These helpers do not implement an inference-engine
connector or make arbitrary model allocations suitable for RDMA.

## Build and install

From the repository root, select the backend available on the target Linux host:

```sh
make -C rpc/gpu cuda CUDA_HOME=/usr/local/cuda
make -C rpc/gpu vulkan
```

CUDA needs the CUDA runtime and driver development headers/libraries and a C++17 compiler;
the wrapper has no kernels and does not need `nvcc`. Override `CUDA_LIBDIR` if
the runtime library is somewhere other than `CUDA_HOME/lib64`. Vulkan needs
Vulkan 1.1 development headers and the loader library. The default output is
`build/rpc/gpu/libmcdma-rpc-cuda.so` or `libmcdma-rpc-vulkan.so`.

The independent install targets are `install-cuda` and `install-vulkan`, with
`PREFIX` and `DESTDIR` support. They install the selected library and its C
headers without changing the core RPC daemon installation. Ensure the runtime
loader can find the selected library and GPU runtime using the host's normal
library configuration. No rpath to a development machine is embedded.

## API and ownership

Both APIs return a status from `mcdma_rpc_gpu.h` and optionally fill a
caller-owned error structure with a backend code and explanatory message.
Each library exports an ABI function returning `MCDMA_RPC_GPU_ABI`, currently 1.
The headers are usable from C; handles are opaque and must not be copied as a
substitute for retaining the original handle.

Pass a complete, writable, caller-owned mailbox mapping or a page-aligned
subspan whose length is a whole number of pages. The code rejects null/empty
spans, pointer arithmetic overflow and incompatible alignment. It cannot prove
that an arbitrary pointer is mapped for the claimed length, owned by the caller
or free of concurrent accesses; the caller must establish those facts.
Do not include memory outside the supplied mapping just to satisfy alignment.

For wrapping, the caller keeps the original mapping alive until release succeeds.
The caller always keeps the GPU device/context alive through release. Serialize
operations on each wrapper and prevent concurrent unmapping,
registration changes, daemon replacement and GPU submissions during teardown.
Release waits for already-submitted GPU work on the relevant device, but cannot
discover outstanding NIC operations. Complete those first through the RPC
protocol, then release the wrapper, then close/unmap the application mailbox.
The explicit Vulkan allocation owns its CPU mapping and releases that mapping
itself; callers must never unmap or free its `host_pointer()` result.
A failed release retains the handle and mapping requirement so it can be retried.
Process shutdown and a lost GPU device require the application's normal recovery;
discarding the handle does not establish that its memory is safe to unmap.

## CUDA

Include `mcdma_rpc_cuda.h`, make the intended CUDA context current, and select
its device before wrapping. For the ordinary primary-context path, call
`cudaSetDevice(device_index)` first, then:

```c
struct mcdma_rpc_cuda *window = NULL;
struct mcdma_rpc_gpu_error error;
int status = mcdma_rpc_cuda_wrap(mailbox, mailbox_bytes, device_index, &window, &error);
if (status == MCDMA_RPC_GPU_OK) {
    void *device_pointer = mcdma_rpc_cuda_device_pointer(window);
    /* Launch work using device_pointer + the mailbox payload offset. */
}
/* After preventing new GPU work and completing the RPC/NIC work: */
if (window != NULL) status = mcdma_rpc_cuda_release(&window, &error);
```

The wrapper uses `cudaHostRegisterMapped` and `cudaHostGetDevicePointer` on exactly
the supplied span. Its device pointer may have a different numerical address
while referring to the same physical allocation. It refuses a mismatched current
device and does not change the application's selected device or context. It
requires an already-active context and captures its exact driver `CUcontext`.
Use the device pointer only in that context, and make that same context current
for release; another context on the same device is refused with
`MCDMA_RPC_GPU_WRONG_CONTEXT`, without synchronization or unregistration.
The original context must remain alive throughout the wrapper's lifetime. An already
CUDA-registered span is an explicit backend error; the wrapper does not take over
someone else's registration or unregister it.

If getting the device pointer fails after registration and unregistering also
fails, wrap returns `MCDMA_RPC_GPU_CLEANUP` and a retained non-null handle. Release
that handle before unmapping the caller memory; its device pointer is unavailable.
Other wrap failures leave the output handle null.

This is mapped host memory, not registration of `cudaMalloc` storage or proof of
GPUDirect RDMA support. Device-memory registration and capabilities need separate
measurement. No library operation silently substitutes host storage for a device
allocation.

## Vulkan

Include `mcdma_rpc_vulkan.h` and supply the application's matching physical and
logical devices. Create the logical device with `VK_EXT_external_memory_host`
enabled, then call:

```c
struct mcdma_rpc_vulkan *window = NULL;
struct mcdma_rpc_gpu_error error;
int status = mcdma_rpc_vulkan_wrap(physical_device, device, mailbox, mailbox_bytes,
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &window, &error);
if (status == MCDMA_RPC_GPU_OK) {
    VkBuffer buffer = mcdma_rpc_vulkan_buffer(window);
    /* Bind a descriptor whose offset/range covers only the intended payload. */
}
/* After preventing new GPU work and completing the RPC/NIC work: */
if (window != NULL) status = mcdma_rpc_vulkan_release(&window, &error);
```

Optional usage bits are `VK_BUFFER_USAGE_TRANSFER_SRC_BIT` and
`VK_BUFFER_USAGE_TRANSFER_DST_BIT`, in addition to the required storage-buffer
bit. The application supplies the exact device; the library does not select a
second device or fall back to a software implementation, and rejects CPU devices.

The import uses `VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT` and the
original host pointer. It requires both HOST_VISIBLE and HOST_COHERENT on a memory
type compatible with the pointer and buffer, observes the device's minimum import
alignment, and refuses buffer requirements extending outside the caller span.
It handles required dedicated allocations without exposing ownership of the
`VkDeviceMemory` object. The caller must not destroy the returned `VkBuffer`.

Coherent memory still needs execution synchronization. Complete a GPU producer
before publishing the mailbox word, wait for the RPC completion before submitting
GPU consumers, and use Vulkan host/compute memory dependencies for the imported
buffer. No shader should access the control page or bytes outside its descriptor
range. These functions do not submit shaders or manage the application pipeline.
See the Vulkan specification's
[host-pointer import ownership and synchronization requirements](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMemoryHostPointerInfoEXT.html).

### Explicit Vulkan allocation and DMA_BUF export

Some drivers cannot import a POSIX shared-memory mailbox as a host pointer but
can export their own coherent allocation. Applications can choose that allocation
path before starting the daemon. `mcdma_rpc_vulkan_allocate_export` creates one
storage buffer, allocates its memory, and maps a CPU view of that same allocation:

```c
struct mcdma_rpc_vulkan *window = NULL;
struct mcdma_rpc_gpu_error error;
int dma_buf_fd = -1;
int status = mcdma_rpc_vulkan_allocate_export(physical_device, device, mailbox_bytes,
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &window, &error);
if (status == MCDMA_RPC_GPU_OK) {
    void *mailbox = mcdma_rpc_vulkan_host_pointer(window);
    VkBuffer buffer = mcdma_rpc_vulkan_buffer(window);
    status = mcdma_rpc_vulkan_export_fd(window, &dma_buf_fd, &error);
    /* Pass the fd to a daemon that maps it and registers this DMA_BUF for RDMA.
     * GPU descriptors refer to buffer; CPU control words use mailbox.
     * Do not proceed with a failed export or failed NIC registration. */
}
/* Stop new GPU submissions, finish NIC work, and deregister every NIC MR first. */
if (dma_buf_fd >= 0) close(dma_buf_fd);
if (window != NULL) status = mcdma_rpc_vulkan_release(&window, &error);
```

Use a hardware Vulkan 1.1 device with `VK_KHR_external_memory_fd` and
`VK_EXT_external_memory_dma_buf` explicitly enabled on the logical device. The
library checks both extensions' physical-device availability and looks up
`vkGetMemoryFdKHR` through the supplied logical device. Vulkan does not expose
the logical device's enabled-extension list, and DMA_BUF adds no entrypoint, so
enabling that extension remains a caller requirement.

The requested length must be a nonzero whole number of system pages. The CPU
mapping must be page aligned, or allocation fails and valid resources are cleaned
up. The driver may require a larger backing allocation; `size()` retains the
requested logical length and callers must stay inside it. Exportable-buffer
capabilities and dedicated-allocation requirements are checked for the requested
usage. Required and preferred dedicated allocations are honored.

Memory selection requires HOST_VISIBLE and HOST_COHERENT, permits only ordinary
DEVICE_LOCAL and HOST_CACHED properties in addition, and prefers non-DEVICE_LOCAL
memory, then HOST_CACHED within that preference. Protected, lazily allocated,
AMD device-coherent/device-uncached, and other special memory types are excluded
because their extra features are not assumed enabled. The selected type gets one
allocation attempt; there is no retry on another type and no staging-buffer or
copy fallback. `wrap()` never switches to this path automatically.

`export_fd()` only accepts handles created by `allocate_export()`. Each successful
call returns a new caller-owned descriptor with close-on-exec set, and every
failure leaves the caller's output fd at `-1`. Passing it across `exec` therefore
requires an explicit inherited-descriptor allowlist. Closing an exported fd is
the caller's responsibility; releasing the wrapper does not close it. Conversely,
holding an exported fd does not permit GPU or NIC use after the wrapper has been
released under this API's lifetime contract.

The allocation handle must remain alive until all GPU/NIC accesses and NIC
registrations have ended. Release waits for GPU work, destroys the buffer, unmaps
its owned CPU view and frees the Vulkan memory object. A failed GPU wait leaves
all of those resources and the handle intact for recovery. Creating an allocation
and exporting its fd do not prove that a particular NIC can register it; the
daemon must verify that separately without silently substituting another buffer.
The same GPU/RDMA execution and memory-ordering requirements apply as for imported
memory. See the Vulkan specification's
[DMA_BUF export allocation requirements](https://docs.vulkan.org/refpages/latest/refpages/source/VkExportMemoryAllocateInfo.html)
and [exported descriptor ownership](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetMemoryFdKHR.html).

## Validation

```sh
make -C rpc/gpu test
make -C rpc/gpu test-vulkan
```

The first target needs only a C++17 compiler and uses CUDA API stubs. It checks
span arithmetic/alignment, device and exact context selection, refusal without fallback, preservation
of caller bytes and registrations on failed cleanup, retry, and partial-create
cleanup. The Vulkan target needs Vulkan headers but no loader or GPU; its stubs
check unsupported/coherence/alignment/size refusal, exact host-pointer import,
dedicated allocation, refusal to destroy poisoned outputs from failed Vulkan
creation/allocation, cleanup of valid resources on failed import/bind, retained resources
after a failed wait, retry and preservation of caller bytes.
It also covers explicit export capability and entrypoint refusal, ordinary-memory
type selection and exclusion of feature-dependent types, driver padding with a
logical length, required/preferred dedicated allocation, one allocation attempt,
mapping failure and alignment, poisoned failed fd outputs, fresh descriptor
ownership, close-on-exec, and retained mappings after a failed release wait.

These offline tests do not establish actual GPU or RDMA operation. Hardware
acceptance must use the production libraries and the daemon's actual allocation,
either imported or explicitly exported, generate and verify payloads on each GPU,
verify guards, exchange them
over the daemon links, and finish with successful release while preserving the
application mapping. Any published result must identify the tested backend,
device, driver, allocation method and synchronization conditions.
