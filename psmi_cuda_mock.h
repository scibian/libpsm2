#ifndef _PSMI_CUDA_MOCK_H
#define _PSMI_CUDA_MOCK_H

#include <stdint.h>

typedef void * CUcontext;

typedef enum _CUresult
{
	CUDA_SUCCESS = 0,
	CUDA_ERROR_INVALID_VALUE = 1,
	CUDA_ERROR_ALREADY_MAPPED = 208,
	CUDA_ERROR_INVALID_HANDLE = 401,
	CUDA_ERROR_NOT_READY = 600
} CUresult;

typedef int CUdevice;
typedef unsigned long long CUdeviceptr;
typedef int CUdevice_attribute;

typedef enum _CUpointer_attribute
{
	CU_POINTER_ATTRIBUTE_MEMORY_TYPE = 2,
	CU_POINTER_ATTRIBUTE_SYNC_MEMOPS = 6,
	CU_POINTER_ATTRIBUTE_IS_MANAGED = 8
} CUpointer_attribute;

typedef int CUevent;

typedef enum _CUmemorytype
{
	CU_MEMORYTYPE_DEVICE = 0x2
} CUmemorytype;

typedef int CUstream;

#define CU_IPC_HANDLE_SIZE 64

typedef struct {
	char reserved[CU_IPC_HANDLE_SIZE];
} CUipcMemHandle;

typedef enum _CUipcMem_flags {
	CU_IPC_MEM_LAZY_ENABLE_PEER_ACCESS = 0x1,
} CUipcMem_flags;

typedef enum _CUevent_flags {
	CU_EVENT_DEFAULT = 0
} CUevent_flags;

typedef enum _CUstream_flags {
	CU_STREAM_NON_BLOCKING = 1
} CUstream_flags;

typedef enum {
	CU_DEVICE_ATTRIBUTE_UNIFIED_ADDRESSING = 41,
	CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR = 75,
	CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR = 76
} CUdevice_attributes;

#define CU_MEMHOSTALLOC_PORTABLE 0x1

#endif /* _PSMI_CUDA_MOCK_H */
