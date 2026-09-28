# GPU tools remain optional; the repository's normal build needs none of them.
CXX ?= c++
NVCC ?= nvcc
HIPCC ?= hipcc
GLSLANG ?= glslangValidator
BUILD ?= build
GPU_API := ../../rpc/gpu
GPU_LIB_DIR := ../../build/rpc/gpu
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wno-missing-field-initializers
NVCCFLAGS ?= -O2 -std=c++17 --cudart shared
HIPFLAGS ?= -O2 -std=c++17

.PHONY: help cuda vulkan hip clean cuda-library vulkan-library mailbox-cuda mailbox-vulkan mailbox-helper
help:
	@echo 'Optional GPU RDMA validation: make cuda, make vulkan, or make hip'
	@echo 'Optional daemon/application GPU test: make mailbox-cuda or make mailbox-vulkan'
	@echo 'Override BUILD, CXX, NVCC, HIPCC, GLSLANG and compiler flags as needed.'

cuda: $(BUILD)/gpu-verbs-cuda
vulkan: $(BUILD)/gpu-verbs-vulkan $(BUILD)/gpu_verbs_shader.spv
hip: $(BUILD)/gpu-verbs-hip

$(BUILD):
	mkdir -p $@

cuda-library:
	$(MAKE) -C $(GPU_API) cuda

vulkan-library:
	$(MAKE) -C $(GPU_API) vulkan

$(BUILD)/gpu-verbs-cuda: gpu_verbs_peer.cu $(GPU_API)/mcdma_rpc_cuda.h cuda-library | $(BUILD)
	$(NVCC) $(NVCCFLAGS) -I$(GPU_API) $< -L$(GPU_LIB_DIR) -lmcdma-rpc-cuda -libverbs -lcuda -Xlinker -rpath -Xlinker '$$ORIGIN' -o $@
	cp $(GPU_LIB_DIR)/libmcdma-rpc-cuda.so $(BUILD)/

$(BUILD)/gpu-verbs-hip: gpu_verbs_peer.cu | $(BUILD)
	$(HIPCC) $(HIPFLAGS) -x hip -DMCDMA_USE_HIP $< -libverbs -o $@

$(BUILD)/gpu-verbs-vulkan: gpu_verbs_vulkan.cpp vulkan_buffer.hpp $(GPU_API)/mcdma_rpc_vulkan.h vulkan-library | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(GPU_API) $< -L$(GPU_LIB_DIR) -lmcdma-rpc-vulkan -lvulkan -libverbs -Wl,-rpath,'$$ORIGIN' -o $@
	cp $(GPU_LIB_DIR)/libmcdma-rpc-vulkan.so $(BUILD)/

$(BUILD)/gpu_verbs_shader.spv: gpu_verbs_shader.comp | $(BUILD)
	$(GLSLANG) -V --target-env vulkan1.1 $< -o $@

clean:
	rm -f $(BUILD)/gpu-verbs-cuda $(BUILD)/gpu-verbs-hip $(BUILD)/gpu-verbs-vulkan $(BUILD)/gpu_verbs_shader.spv $(BUILD)/libmcdma-rpc-cuda.so $(BUILD)/libmcdma-rpc-vulkan.so $(BUILD)/libmcdma-rpc.so $(BUILD)/gpu-mailbox-cuda $(BUILD)/gpu-mailbox-vulkan $(BUILD)/gpu_mailbox_shader.spv

# Existing shared-mailbox imports; the application opens no verbs context.
mailbox-cuda: $(BUILD)/gpu-mailbox-cuda
mailbox-vulkan: $(BUILD)/gpu-mailbox-vulkan $(BUILD)/gpu_mailbox_shader.spv

mailbox-helper:
	$(MAKE) -C ../../rpc ../build/rpc/libmcdma-rpc.so

$(BUILD)/gpu-mailbox-cuda: gpu_mailbox_cuda.cu mailbox_app.hpp mailbox_support.hpp spawned_daemon.hpp cuda-library mailbox-helper | $(BUILD)
	$(NVCC) $(NVCCFLAGS) -I$(GPU_API) -I../../rpc $< -L$(GPU_LIB_DIR) -L../../build/rpc -lmcdma-rpc-cuda -lmcdma-rpc -Xlinker -rpath -Xlinker '$$ORIGIN' -o $@
	cp $(GPU_LIB_DIR)/libmcdma-rpc-cuda.so ../../build/rpc/libmcdma-rpc.so $(BUILD)/

$(BUILD)/gpu-mailbox-vulkan: gpu_mailbox_vulkan.cpp mailbox_app.hpp mailbox_support.hpp spawned_daemon.hpp vulkan-library mailbox-helper | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(GPU_API) -I../../rpc $< -L$(GPU_LIB_DIR) -L../../build/rpc -lmcdma-rpc-vulkan -lmcdma-rpc -lvulkan -Wl,-rpath,'$$ORIGIN' -o $@
	cp $(GPU_LIB_DIR)/libmcdma-rpc-vulkan.so ../../build/rpc/libmcdma-rpc.so $(BUILD)/

$(BUILD)/gpu_mailbox_shader.spv: gpu_mailbox_shader.comp | $(BUILD)
	$(GLSLANG) -V --target-env vulkan1.1 $< -o $@
