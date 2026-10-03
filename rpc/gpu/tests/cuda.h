/* Driver API test double; the optional library links the real CUDA driver. */
#ifndef TEST_CUDA_DRIVER_H
#define TEST_CUDA_DRIVER_H
typedef struct CUctx_st *CUcontext;
typedef enum { CUDA_SUCCESS = 0, CUDA_ERROR_NOT_INITIALIZED = 3 } CUresult;
#ifdef __cplusplus
extern "C" {
#endif
CUresult cuCtxGetCurrent(CUcontext *);
CUresult cuGetErrorString(CUresult, const char **);
#ifdef __cplusplus
}
#endif
#endif
