# Optional Linux GPU imports; the core rpc build never invokes these targets.
CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror
CPPFLAGS ?=
LDFLAGS ?=
CUDA_HOME ?= /usr/local/cuda
CUDA_LIBDIR ?= $(CUDA_HOME)/lib64
OUT ?= ../../build/rpc/gpu
PREFIX ?= /usr/local
SHARED := -shared -fPIC -fvisibility=hidden
HEADERS := mcdma_rpc_gpu.h gpu_internal.hpp

help:
	@echo 'Optional targets: cuda, vulkan, test, test-vulkan, install-cuda, install-vulkan'
	@echo 'CUDA_HOME/CUDA_LIBDIR locate the CUDA runtime; Vulkan uses system development headers.'

$(OUT):
	mkdir -p $(OUT)

cuda: $(OUT)/libmcdma-rpc-cuda.so
vulkan: $(OUT)/libmcdma-rpc-vulkan.so

$(OUT)/libmcdma-rpc-cuda.so: libmcdma_rpc_cuda.cpp mcdma_rpc_cuda.h $(HEADERS) | $(OUT)
	test "$$(uname -s)" = Linux
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SHARED) -I$(CUDA_HOME)/include $< $(LDFLAGS) -L$(CUDA_LIBDIR) -Wl,-z,defs -lcudart -lcuda -o $@

$(OUT)/libmcdma-rpc-vulkan.so: libmcdma_rpc_vulkan.cpp mcdma_rpc_vulkan.h $(HEADERS) | $(OUT)
	test "$$(uname -s)" = Linux
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SHARED) $< $(LDFLAGS) -Wl,-z,defs -lvulkan -o $@

# These tests use CUDA stubs and need neither a CUDA SDK nor a GPU.
test: $(OUT)/test-span $(OUT)/test-cuda
	$(OUT)/test-span
	$(OUT)/test-cuda

$(OUT)/test-span: tests/test_span.cpp $(HEADERS) | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

$(OUT)/test-cuda: tests/test_cuda.cpp tests/cuda_runtime_api.h tests/cuda.h libmcdma_rpc_cuda.cpp mcdma_rpc_cuda.h $(HEADERS) | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Itests tests/test_cuda.cpp libmcdma_rpc_cuda.cpp -o $@

# Requires Vulkan development headers, but links API stubs and needs no GPU.
test-vulkan: $(OUT)/test-vulkan
	$(OUT)/test-vulkan

$(OUT)/test-vulkan: tests/test_vulkan.cpp libmcdma_rpc_vulkan.cpp mcdma_rpc_vulkan.h $(HEADERS) | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_vulkan.cpp libmcdma_rpc_vulkan.cpp -o $@

install-cuda: cuda
	install -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include
	install -m 0755 $(OUT)/libmcdma-rpc-cuda.so $(DESTDIR)$(PREFIX)/lib/
	install -m 0644 mcdma_rpc_gpu.h mcdma_rpc_cuda.h $(DESTDIR)$(PREFIX)/include/

install-vulkan: vulkan
	install -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include
	install -m 0755 $(OUT)/libmcdma-rpc-vulkan.so $(DESTDIR)$(PREFIX)/lib/
	install -m 0644 mcdma_rpc_gpu.h mcdma_rpc_vulkan.h $(DESTDIR)$(PREFIX)/include/

.PHONY: help cuda vulkan test test-vulkan install-cuda install-vulkan
