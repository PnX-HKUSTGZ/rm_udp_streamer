#include "device.hpp"
#include "interop/gpu_api.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <regex>
#include <sstream>
#include <stdexcept>
extern "C" {
#include <libavcodec/avcodec.h>
typedef rm::gpu::Context CUcontext;
typedef rm::gpu::Stream CUstream;
#define CUDA_VERSION 7050
#include <libavutil/hwcontext_cuda.h>
}
#ifdef _WIN32
#include <libavutil/hwcontext_d3d11va.h>
#include <dxgi1_2.h>
#else
#include <libavutil/hwcontext_drm.h>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#endif

namespace rm {
namespace {
bool numeric(const std::string &s) {
    return !s.empty() && s.size() <= 3 && s.find_first_not_of("0123456789") == std::string::npos;
}
bool matches(const std::string &s, const char *pattern) { return std::regex_match(s, std::regex(pattern)); }
std::string lower(std::string s) {
    std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return char(std::tolower(c));}); return s;
}
void check(int code, const char *operation) {
    if (code >= 0) return;
    char message[AV_ERROR_MAX_STRING_SIZE]{}; av_strerror(code,message,sizeof(message));
    throw std::runtime_error(std::string(operation)+": "+message);
}
AVPixelFormat backend_format(const std::string &backend) {
    const auto *codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    const auto type = av_hwdevice_find_type_by_name(backend.c_str());
    if (!codec || type == AV_HWDEVICE_TYPE_NONE) return AV_PIX_FMT_NONE;
    for (int i=0; const auto *entry=avcodec_get_hw_config(codec,i); ++i)
        if (entry->device_type==type && (entry->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) return entry->pix_fmt;
    return AV_PIX_FMT_NONE;
}
struct Buffer {
    AVBufferRef *value = nullptr;
    ~Buffer(){av_buffer_unref(&value);}
};
struct Cuda {
    struct Uuid { unsigned char bytes[16]; };
#define FN(name, args) using T##name=int(RM_GPU_CALL *) args; T##name name=nullptr
    FN(cuInit,(unsigned)); FN(cuDeviceGetCount,(int *)); FN(cuDeviceGet,(int *,int));
    FN(cuDeviceGetName,(char *,int,int)); FN(cuDeviceGetUuid,(Uuid *,int));
    FN(cuCtxPushCurrent_v2,(gpu::Context)); FN(cuCtxPopCurrent_v2,(gpu::Context *)); FN(cuCtxGetDevice,(int *));
#undef FN
    Cuda() {
#define LOAD(name) name=reinterpret_cast<T##name>(gpu::symbol("nvcuda.dll",#name)); if(!name)throw std::runtime_error("CUDA device API unavailable: " #name)
        LOAD(cuInit); LOAD(cuDeviceGetCount); LOAD(cuDeviceGet); LOAD(cuDeviceGetName);
        LOAD(cuCtxPushCurrent_v2); LOAD(cuCtxPopCurrent_v2); LOAD(cuCtxGetDevice);
#undef LOAD
        cuDeviceGetUuid=reinterpret_cast<TcuDeviceGetUuid>(gpu::symbol("nvcuda.dll","cuDeviceGetUuid_v2"));
        if(!cuDeviceGetUuid)cuDeviceGetUuid=reinterpret_cast<TcuDeviceGetUuid>(gpu::symbol("nvcuda.dll","cuDeviceGetUuid"));
        if(!cuDeviceGetUuid || cuInit(0))throw std::runtime_error("CUDA device enumeration unavailable");
    }
    DeviceInfo describe(int device, int ordinal) const {
        char label[256]{}; Uuid uuid{};
        if(cuDeviceGetName(label,sizeof(label),device) || cuDeviceGetUuid(&uuid,device))
            throw std::runtime_error("Cannot read actual CUDA device name/UUID");
        std::ostringstream id; id<<"GPU-"<<std::hex<<std::setfill('0');
        for(int i=0;i<16;++i){if(i==4||i==6||i==8||i==10)id<<'-';id<<std::setw(2)<<unsigned(uuid.bytes[i]);}
        DeviceInfo out; out.backend="cuda";out.ordinal=ordinal;out.name=label;out.selector="cuda:"+id.str();
        out.identity=out.name+" CUDA="+std::to_string(ordinal)+" UUID="+id.str();return out;
    }
};
std::vector<DeviceInfo> cuda_devices() {
    Cuda api; int count=0;
    if(api.cuDeviceGetCount(&count))throw std::runtime_error("Cannot enumerate CUDA devices");
    std::vector<DeviceInfo> result;
    for(int ordinal=0;ordinal<count;++ordinal){int device=-1;
        if(api.cuDeviceGet(&device,ordinal))throw std::runtime_error("Cannot resolve CUDA ordinal");
        result.push_back(api.describe(device,ordinal));
    }
    return result;
}
DeviceInfo actual_cuda(AVBufferRef *ref) {
    Cuda api; auto *hw=reinterpret_cast<AVHWDeviceContext *>(ref->data);
    auto *cuda=reinterpret_cast<AVCUDADeviceContext *>(hw->hwctx);
    if(api.cuCtxPushCurrent_v2(cuda->cuda_ctx))throw std::runtime_error("Cannot query CUDA context identity");
    int device=-1; int status=api.cuCtxGetDevice(&device); DeviceInfo result;
    try {
        if(status)throw std::runtime_error("Cannot query actual CUDA device");
        result=api.describe(device,device);
    } catch(...) {gpu::Context previous=nullptr;api.cuCtxPopCurrent_v2(&previous);throw;}
    gpu::Context previous=nullptr;
    if(api.cuCtxPopCurrent_v2(&previous))throw std::runtime_error("Cannot restore CUDA context");
    return result;
}
#ifdef _WIN32
template<class T> struct Com {
    T *value=nullptr;
    ~Com(){if(value)value->Release();}
};
DeviceInfo describe_adapter(IDXGIAdapter *adapter, int ordinal) {
    DXGI_ADAPTER_DESC desc{};
    if(FAILED(adapter->GetDesc(&desc)))throw std::runtime_error("Cannot query D3D11 adapter identity");
    char label[512]{};
    if(!WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,label,sizeof(label),nullptr,nullptr))
        throw std::runtime_error("Cannot read D3D11 adapter name");
    DeviceInfo out;out.backend="d3d11va";out.ordinal=ordinal;out.name=label;
    auto luid=std::to_string(desc.AdapterLuid.HighPart)+":"+std::to_string(desc.AdapterLuid.LowPart);
    out.selector="d3d11va:LUID="+luid;out.identity=out.name+" LUID="+luid;return out;
}
std::vector<DeviceInfo> d3d_devices() {
    using CreateFactory=HRESULT(WINAPI *)(REFIID,void **);
    auto create=reinterpret_cast<CreateFactory>(gpu::symbol("dxgi.dll","CreateDXGIFactory1"));
    Com<IDXGIFactory1> factory;
    if(!create||FAILED(create(__uuidof(IDXGIFactory1),reinterpret_cast<void **>(&factory.value))))
        throw std::runtime_error("Cannot enumerate D3D11 adapters");
    std::vector<DeviceInfo> result;
    for(UINT index=0;;++index){
        Com<IDXGIAdapter> adapter;auto status=factory.value->EnumAdapters(index,&adapter.value);
        if(status==DXGI_ERROR_NOT_FOUND)break;
        if(FAILED(status)||!adapter.value)throw std::runtime_error("D3D11 enumeration failed");
        result.push_back(describe_adapter(adapter.value,int(index)));
    }
    return result;
}
DeviceInfo actual_d3d(AVBufferRef *ref) {
    auto *hw=reinterpret_cast<AVHWDeviceContext *>(ref->data);
    auto *d3d=reinterpret_cast<AVD3D11VADeviceContext *>(hw->hwctx);
    Com<IDXGIDevice> dxgi;Com<IDXGIAdapter> adapter;
    if(FAILED(d3d->device->QueryInterface(__uuidof(IDXGIDevice),reinterpret_cast<void **>(&dxgi.value))) ||
       FAILED(dxgi.value->GetAdapter(&adapter.value)))throw std::runtime_error("Cannot identify actual D3D11 adapter");
    return describe_adapter(adapter.value,-1);
}
#else
namespace fs=std::filesystem;
std::string read_text(const fs::path &p) {std::ifstream input(p);std::string value;input>>value;return value;}
DeviceInfo describe_drm(dev_t number) {
    auto sys=fs::canonical(fs::path("/sys/dev/char")/(std::to_string(major(number))+":"+std::to_string(minor(number))));
    if(!matches(sys.filename().string(),"renderD[0-9]+"))throw std::runtime_error("VAAPI requires a DRM render node");
    auto physical=fs::canonical(sys/"device");
    DeviceInfo out;out.backend="vaapi";out.device_number=uint64_t(number);out.node="/dev/dri/"+sys.filename().string();
    auto pci=physical.filename().string();
    if(matches(pci,"[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\\.[0-7]"))out.pci=lower(pci);
    out.name="DRM "+read_text(physical/"vendor")+":"+read_text(physical/"device");
    out.selector=out.pci.empty()?"vaapi:"+out.node:"vaapi:pci:"+out.pci;
    out.identity=out.name+" PCI="+out.pci+" NODE="+out.node+" DRM="+std::to_string(major(number))+":"+std::to_string(minor(number));
    return out;
}
DeviceInfo describe_node(const std::string &node) {
    struct stat info{};
    if(stat(node.c_str(),&info)!=0)throw std::runtime_error("Cannot stat DRM node "+node+": "+std::strerror(errno));
    if(!S_ISCHR(info.st_mode))throw std::runtime_error("VAAPI device is not a character device: "+node);
    auto result=describe_drm(info.st_rdev);
    if(access(node.c_str(),R_OK|W_OK)!=0)result.error="DRM node access denied: "+node+": "+std::strerror(errno);
    return result;
}
std::vector<DeviceInfo> vaapi_devices() {
    if(!fs::exists("/dev/dri"))return {};
    std::vector<std::string> nodes;
    for(const auto &entry:fs::directory_iterator("/dev/dri"))
        if(matches(entry.path().filename().string(),"renderD[0-9]+"))nodes.push_back(entry.path().string());
    std::sort(nodes.begin(),nodes.end());
    std::vector<DeviceInfo> result;
    for(const auto &node:nodes){
        DeviceInfo item;
        try{item=describe_node(node);}catch(const std::exception &e){item.backend="vaapi";item.node=node;item.selector="vaapi:"+node;item.error=e.what();}
        item.ordinal=int(result.size());result.push_back(std::move(item));
    }
    return result;
}
#endif
std::vector<DeviceInfo> devices_for(const std::string &backend) {
    if(backend=="cuda")return cuda_devices();
#ifdef _WIN32
    if(backend=="d3d11va")return d3d_devices();
#else
    if(backend=="vaapi")return vaapi_devices();
#endif
    throw std::runtime_error("Backend is unavailable on this platform: "+backend);
}
DeviceInfo resolve(const std::string &backend,const std::string &selector) {
#ifndef _WIN32
    if(backend=="vaapi"){
        auto path=selector.rfind("vaapi:",0)==0?selector.substr(6):selector;
        if(path.rfind("/dev/dri/",0)==0){auto result=describe_node(path);if(!result.error.empty())throw std::runtime_error(result.error);return result;}
    }
#endif
    auto devices=devices_for(backend);
    std::string wanted=selector;
    if(selector.rfind("GPU-",0)==0)wanted="cuda:"+selector;
    for(const auto &entry:devices){
        bool selected=numeric(selector)?entry.ordinal==std::stoi(selector):lower(entry.selector)==lower(wanted);
        if(!selected)continue;
        if(!entry.error.empty())throw std::runtime_error(entry.error);
        return entry;
    }
    throw std::runtime_error("Requested "+backend+" device does not exist: "+selector);
}
}
std::string selector_backend(const std::string &selector) {
    if(selector.rfind("cuda:",0)==0||selector.rfind("GPU-",0)==0)return "cuda";
    if(selector.rfind("d3d11va:",0)==0)return "d3d11va";
    if(selector.rfind("vaapi:",0)==0||selector.rfind("/dev/dri/",0)==0)return "vaapi";
    return {};
}
std::string validate_device_selector(const std::string &backend,const std::string &selector) {
    if(selector.empty()||selector.size()>512||selector.find('\0')!=std::string::npos)return "Invalid device selector length";
    if(numeric(selector))return {};
    auto scope=selector_backend(selector);
    if(scope.empty())return "Device must be an index (0..999), CUDA UUID, D3D11 LUID or VAAPI render/PCI path";
    if(backend!="auto"&&backend!=scope)return "Device identity belongs to "+scope+", not "+backend;
    if(scope=="cuda"){
        auto id=selector.rfind("cuda:",0)==0?selector.substr(5):selector;
        if(matches(id,"GPU-[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}"))return {};
    }else if(scope=="d3d11va"){
        if(matches(selector,"d3d11va:LUID=-?[0-9]{1,10}:[0-9]{1,10}")){
            auto colon=selector.find(':',13);
            try{auto high=std::stoll(selector.substr(13,colon-13));auto low=std::stoull(selector.substr(colon+1));
                if(high>=INT32_MIN&&high<=INT32_MAX&&low<=UINT32_MAX)return {};
            }catch(const std::exception &){}
        }
    }else{
        auto path=selector.rfind("vaapi:",0)==0?selector.substr(6):selector;
        if(matches(path,"pci:[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\\.[0-7]")||
           matches(path,"/dev/dri/renderD[0-9]+")||matches(path,"/dev/dri/by-path/[A-Za-z0-9_:.-]+-render"))return {};
    }
    return "Malformed "+scope+" device identity";
}
std::vector<std::string> decoder_candidates(const std::string &backend,const std::string &transfer,const std::string &selector) {
    if(backend!="auto")return {backend};
    auto scope=selector_backend(selector);
    if(!scope.empty())return {scope,"software"};
#ifdef _WIN32
    return transfer=="auto"?std::vector<std::string>{"cuda","d3d11va","software"}:std::vector<std::string>{"d3d11va","cuda","software"};
#else
    (void)transfer;return {"cuda","vaapi","software"};
#endif
}
std::vector<BackendInfo> enumerate_backends() {
    std::vector<BackendInfo> result;
    for(const char *name:{"software","cuda","d3d11va","vaapi"}){
        BackendInfo item;item.name=name;item.compiled=item.name=="software"?avcodec_find_decoder(AV_CODEC_ID_HEVC)!=nullptr:backend_format(name)!=AV_PIX_FMT_NONE;
        if(!item.compiled)item.error="Backend absent from fixed FFmpeg build";
        else if(item.name!="software"){
            try{item.devices=devices_for(name);if(item.devices.empty())item.error="No visible devices";}
            catch(const std::exception &e){item.error=e.what();}
        }
        result.push_back(std::move(item));
    }
    return result;
}
void open_hardware_device(const std::string &backend,const std::string &selector,AVBufferRef **device,
                          AVPixelFormat &format,std::string &identity) {
    if(!device||*device)throw std::invalid_argument("Hardware device output must be empty");
    auto invalid=validate_device_selector(backend,selector);if(!invalid.empty())throw std::invalid_argument(invalid);
    format=backend_format(backend);
    if(format==AV_PIX_FMT_NONE)throw std::runtime_error("Backend absent from fixed FFmpeg build: "+backend);
    auto requested=resolve(backend,selector);Buffer opened;
    DeviceInfo actual;
#ifndef _WIN32
    if(backend=="vaapi"){
        Buffer drm;
        check(av_hwdevice_ctx_create(&drm.value,AV_HWDEVICE_TYPE_DRM,requested.node.c_str(),nullptr,0),"open DRM render device");
        auto *hw=reinterpret_cast<AVHWDeviceContext *>(drm.value->data);
        auto *context=reinterpret_cast<AVDRMDeviceContext *>(hw->hwctx);struct stat info{};
        if(fstat(context->fd,&info)!=0||!S_ISCHR(info.st_mode))throw std::runtime_error("Cannot identify opened DRM render device");
        actual=describe_drm(info.st_rdev);
        if(actual.device_number!=requested.device_number||actual.pci!=requested.pci)
            throw std::runtime_error("Actual DRM device differs from requested identity");
        // FFmpeg retains the source DRM context until VAAPI frames/device refs
        // are released. It terminates VA before closing the source device fd.
        check(av_hwdevice_ctx_create_derived(&opened.value,AV_HWDEVICE_TYPE_VAAPI,drm.value,0),"derive VAAPI device from DRM");
    }else
#endif
    {
        auto index=std::to_string(requested.ordinal);
        check(av_hwdevice_ctx_create(&opened.value,av_hwdevice_find_type_by_name(backend.c_str()),index.c_str(),nullptr,0),"create hardware device");
        if(backend=="cuda")actual=actual_cuda(opened.value);
#ifdef _WIN32
        else if(backend=="d3d11va")actual=actual_d3d(opened.value);
#endif
        if(actual.selector!=requested.selector)throw std::runtime_error("Actual hardware device differs from requested identity");
    }
    identity=actual.identity;*device=opened.value;opened.value=nullptr;
}
}
