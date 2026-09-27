#pragma once
#include <cstddef>
#include <cstdint>

using aclError = int;
constexpr aclError ACL_SUCCESS = 0;
constexpr int ACL_FLOAT = 0;
constexpr int ACL_MEM_MALLOC_NORMAL_ONLY = 0;
enum aclrtRunMode { ACL_HOST, ACL_DEVICE };
enum aclrtMemcpyKind { ACL_MEMCPY_HOST_TO_DEVICE, ACL_MEMCPY_DEVICE_TO_HOST };
struct FakeContext;
struct FakeStream;
struct aclmdlDesc;
struct aclmdlDataset;
struct aclDataBuffer;
using aclrtContext = FakeContext*;
using aclrtStream = FakeStream*;
struct aclmdlIODims { size_t dimCount; int64_t dims[8]; };

aclError aclInit(const char*);
aclError aclFinalize();
aclError aclrtSetDevice(int);
aclError aclrtResetDevice(int);
aclError aclrtCreateContext(aclrtContext*, int);
aclError aclrtDestroyContext(aclrtContext);
aclError aclrtSetCurrentContext(aclrtContext);
aclError aclrtGetRunMode(aclrtRunMode*);
aclError aclrtCreateStream(aclrtStream*);
aclError aclrtDestroyStream(aclrtStream);
aclError aclrtSynchronizeStream(aclrtStream);
aclError aclrtMalloc(void**, size_t, int);
aclError aclrtFree(void*);
aclError aclrtMallocHost(void**, size_t);
aclError aclrtFreeHost(void*);
aclError aclrtMemcpy(void*, size_t, const void*, size_t, aclrtMemcpyKind);
aclError aclrtMemcpyAsync(void*, size_t, const void*, size_t, aclrtMemcpyKind, aclrtStream);
aclDataBuffer* aclCreateDataBuffer(void*, size_t);
aclError aclDestroyDataBuffer(aclDataBuffer*);
aclmdlDataset* aclmdlCreateDataset();
aclError aclmdlDestroyDataset(aclmdlDataset*);
aclError aclmdlAddDatasetBuffer(aclmdlDataset*, aclDataBuffer*);
aclError aclmdlLoadFromFile(const char*, uint32_t*);
aclError aclmdlUnload(uint32_t);
aclmdlDesc* aclmdlCreateDesc();
aclError aclmdlDestroyDesc(aclmdlDesc*);
aclError aclmdlGetDesc(aclmdlDesc*, uint32_t);
size_t aclmdlGetNumInputs(const aclmdlDesc*);
size_t aclmdlGetNumOutputs(const aclmdlDesc*);
int aclmdlGetInputDataType(const aclmdlDesc*, size_t);
int aclmdlGetOutputDataType(const aclmdlDesc*, size_t);
aclError aclmdlGetInputDims(const aclmdlDesc*, size_t, aclmdlIODims*);
aclError aclmdlGetOutputDims(const aclmdlDesc*, size_t, aclmdlIODims*);
size_t aclmdlGetInputSizeByIndex(const aclmdlDesc*, size_t);
size_t aclmdlGetOutputSizeByIndex(const aclmdlDesc*, size_t);
const char* aclmdlGetInputNameByIndex(const aclmdlDesc*, size_t);
const char* aclmdlGetOutputNameByIndex(const aclmdlDesc*, size_t);
aclError aclmdlExecute(uint32_t, const aclmdlDataset*, aclmdlDataset*);
aclError aclmdlExecuteAsync(uint32_t, const aclmdlDataset*, aclmdlDataset*, aclrtStream);
