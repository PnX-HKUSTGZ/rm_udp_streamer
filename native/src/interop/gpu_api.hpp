#pragma once
// Small driver ABI surface, verified against pinned nv-codec-headers and GL 3.3.
// No CUDA toolkit or link-time vendor dependency is required.
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define RM_GPU_CALL __stdcall
#else
#include <dlfcn.h>
#define RM_GPU_CALL
#endif
namespace rm::gpu {
using Context = void *; using Stream = void *; using Event = void *;
using Resource = void *; using Array = void *; using Sync = void *;
using DevicePtr = unsigned long long;
struct Copy2D {
    size_t src_x, src_y; unsigned src_type; const void *src_host; DevicePtr src_device;
    Array src_array; size_t src_pitch;
    size_t dst_x, dst_y; unsigned dst_type; void *dst_host; DevicePtr dst_device;
    Array dst_array; size_t dst_pitch, width, height;
};
static_assert(sizeof(Copy2D) == 128, "CUDA_MEMCPY2D ABI requires x86_64");
inline void *symbol(const char *library, const char *name, bool gl = false) {
#ifdef _WIN32
    HMODULE module = GetModuleHandleA(library);
    if (!module) module = LoadLibraryA(library);
    if (!module) return nullptr;
    void *result = nullptr;
    if (gl) {
        auto get = reinterpret_cast<PROC(WINAPI *)(LPCSTR)>(GetProcAddress(module,"wglGetProcAddress"));
        if (get) result = reinterpret_cast<void *>(get(name));
        if (uintptr_t(result) <= 3 || uintptr_t(result) == UINTPTR_MAX) result = nullptr;
    }
    return result ? result : reinterpret_cast<void *>(GetProcAddress(module,name));
#else
    // Process-lifetime library handles: function pointers remain valid for late retirement.
    static void *cuda = dlopen("libcuda.so.1",RTLD_NOW|RTLD_LOCAL);
    static void *graphics = dlopen("libGL.so.1",RTLD_NOW|RTLD_LOCAL);
    (void)library; auto module = gl ? graphics : cuda;
    if (!module) return nullptr;
    auto get = reinterpret_cast<void *(*)(const unsigned char *)>(dlsym(module,"glXGetProcAddressARB"));
    return gl && get ? get(reinterpret_cast<const unsigned char *>(name)) : dlsym(module,name);
#endif
}
struct Driver {
#define FN(name, args) using T##name = int(RM_GPU_CALL *) args; T##name name = nullptr
    FN(cuInit,(unsigned));
    FN(cuCtxPushCurrent_v2,(Context)); FN(cuCtxPopCurrent_v2,(Context *)); FN(cuCtxGetDevice,(int *));
    FN(cuGLGetDevices,(unsigned *,int *,unsigned,unsigned));
    FN(cuGraphicsGLRegisterImage,(Resource *,unsigned,unsigned,unsigned));
    FN(cuGraphicsMapResources,(unsigned,Resource *,Stream));
    FN(cuGraphicsUnmapResources,(unsigned,Resource *,Stream));
    FN(cuGraphicsSubResourceGetMappedArray,(Array *,Resource,unsigned,unsigned));
    FN(cuGraphicsUnregisterResource,(Resource));
    FN(cuMemcpy2DAsync_v2,(const Copy2D *,Stream));
    FN(cuEventCreate,(Event *,unsigned)); FN(cuEventRecord,(Event,Stream));
    FN(cuEventQuery,(Event)); FN(cuEventDestroy_v2,(Event));
    FN(cuGetErrorName,(int,const char **));
#undef FN
    void load() {
#define LOAD(name) name = reinterpret_cast<T##name>(symbol("nvcuda.dll",#name)); if (!name) throw std::runtime_error("CUDA driver lacks " #name)
        LOAD(cuInit); LOAD(cuCtxPushCurrent_v2); LOAD(cuCtxPopCurrent_v2); LOAD(cuCtxGetDevice); LOAD(cuGLGetDevices);
        LOAD(cuGraphicsGLRegisterImage); LOAD(cuGraphicsMapResources); LOAD(cuGraphicsUnmapResources);
        LOAD(cuGraphicsSubResourceGetMappedArray); LOAD(cuGraphicsUnregisterResource); LOAD(cuMemcpy2DAsync_v2);
        LOAD(cuEventCreate); LOAD(cuEventRecord); LOAD(cuEventQuery); LOAD(cuEventDestroy_v2); LOAD(cuGetErrorName);
#undef LOAD
    }
    void check(int result,const char *operation) const {
        if (!result) return;
        const char *label = nullptr; if (cuGetErrorName) cuGetErrorName(result,&label);
        throw std::runtime_error(std::string(operation)+": "+(label?label:std::to_string(result)));
    }
};
struct Gl {
#define FN(name, ret, args) using T##name = ret(RM_GPU_CALL *) args; T##name name = nullptr
    FN(GetIntegerv,void,(unsigned,int *)); FN(GetError,unsigned,());
    FN(GetStringi,const unsigned char *,(unsigned,unsigned));
    FN(GenTextures,void,(int,unsigned *)); FN(BindTexture,void,(unsigned,unsigned));
    FN(BindBuffer,void,(unsigned,unsigned)); FN(TexParameteri,void,(unsigned,unsigned,int));
    FN(TexImage2D,void,(unsigned,int,int,int,int,int,unsigned,unsigned,const void *));
    FN(DeleteTextures,void,(int,const unsigned *));
    FN(FenceSync,Sync,(unsigned,unsigned)); FN(ClientWaitSync,unsigned,(Sync,unsigned,uint64_t));
    FN(DeleteSync,void,(Sync)); FN(Flush,void,());
    FN(CopyImageSubData,void,(unsigned,unsigned,int,int,int,int,unsigned,unsigned,int,int,int,int,int,int,int));
#undef FN
    void load() {
#define LOAD(name) name = reinterpret_cast<T##name>(symbol("opengl32.dll","gl" #name,true)); if (!name) throw std::runtime_error("OpenGL lacks gl" #name)
        LOAD(GetIntegerv); LOAD(GetError); LOAD(GenTextures); LOAD(BindTexture); LOAD(BindBuffer);
        LOAD(TexParameteri); LOAD(TexImage2D); LOAD(DeleteTextures); LOAD(FenceSync); LOAD(ClientWaitSync); LOAD(DeleteSync); LOAD(Flush);
        LOAD(GetStringi);LOAD(CopyImageSubData);
#undef LOAD
    }
};
struct CurrentContext {
    Driver &driver;
    CurrentContext(Driver &d,Context context):driver(d) { driver.check(driver.cuCtxPushCurrent_v2(context),"push CUDA context"); }
    ~CurrentContext() { Context ignored; driver.cuCtxPopCurrent_v2(&ignored); }
};
}
