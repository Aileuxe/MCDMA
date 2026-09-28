/* CUDA API test double, only used by the offline test target. */
#ifndef TEST_CUDA_RUNTIME_API_H
#define TEST_CUDA_RUNTIME_API_H
#include <stddef.h>
typedef enum { cudaSuccess = 0, cudaErrorInvalidValue = 1, cudaErrorUnknown = 999 } cudaError_t;
typedef enum { cudaDevAttrCanMapHostMemory = 19 } cudaDeviceAttr;
#define cudaHostRegisterMapped 2u
#ifdef __cplusplus
extern "C" {
#endif
cudaError_t cudaGetDevice(int *);
cudaError_t cudaDeviceGetAttribute(int *, cudaDeviceAttr, int);
cudaError_t cudaHostRegister(void *, size_t, unsigned);
cudaError_t cudaHostGetDevicePointer(void **, void *, unsigned);
cudaError_t cudaHostUnregister(void *);
cudaError_t cudaDeviceSynchronize(void);
const char *cudaGetErrorString(cudaError_t);
#ifdef __cplusplus
}
#endif
#endif
