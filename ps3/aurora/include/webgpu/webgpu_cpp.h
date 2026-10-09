// PS3 build: inert stand-ins for the WebGPU (Dawn) C++ types that aurora's
// shared GX headers mention. The PS3 renders through the RSX backend in
// ps3/aurora/, so none of these objects are ever created; they only need to
// exist so the GX front end (state tracking, command processor, texture
// caches) compiles unchanged.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

typedef struct WGPUBindGroupImpl* WGPUBindGroup;
typedef struct WGPUBindGroupDescriptor {
    const void* nextInChain = nullptr;
    const char* label = nullptr;
    void* layout = nullptr;
    size_t entryCount = 0;
    const void* entries = nullptr;
} WGPUBindGroupDescriptor;
#define WGPU_STRLEN SIZE_MAX

namespace wgpu {

#define MKW_PS3_WGPU_HANDLE(Name)                                                                                  \
    class Name {                                                                                                   \
    public:                                                                                                        \
        Name() = default;                                                                                          \
        Name(std::nullptr_t) {}                                                                                    \
        explicit operator bool() const { return false; }                                                           \
        bool operator==(const Name&) const { return true; }                                                        \
        bool operator!=(const Name&) const { return false; }                                                       \
        void* Get() const { return nullptr; }                                                                      \
    };

MKW_PS3_WGPU_HANDLE(Buffer)
MKW_PS3_WGPU_HANDLE(BindGroup)
MKW_PS3_WGPU_HANDLE(BindGroupLayout)
MKW_PS3_WGPU_HANDLE(Texture)
MKW_PS3_WGPU_HANDLE(TextureView)
MKW_PS3_WGPU_HANDLE(Sampler)
MKW_PS3_WGPU_HANDLE(RenderPipeline)
MKW_PS3_WGPU_HANDLE(ShaderModule)
MKW_PS3_WGPU_HANDLE(CommandEncoder)
MKW_PS3_WGPU_HANDLE(RenderPassEncoder)
MKW_PS3_WGPU_HANDLE(CommandBuffer)
MKW_PS3_WGPU_HANDLE(Device)
MKW_PS3_WGPU_HANDLE(Queue)
MKW_PS3_WGPU_HANDLE(Surface)
MKW_PS3_WGPU_HANDLE(Instance)
MKW_PS3_WGPU_HANDLE(PipelineLayout)
#undef MKW_PS3_WGPU_HANDLE

enum class TextureFormat : uint32_t {
    Undefined,
    R8Unorm,
    RG8Unorm,
    R16Sint,
    RGBA8Unorm,
    BGRA8Unorm,
    RGBA16Float,
    BC1RGBAUnorm,
    Depth24Plus,
    Depth32Float,
};
enum class BackendType : uint32_t { Undefined, Null };
enum class FilterMode : uint32_t { Undefined, Nearest, Linear };
enum class MipmapFilterMode : uint32_t { Undefined, Nearest, Linear };
enum class AddressMode : uint32_t { Undefined, ClampToEdge, Repeat, MirrorRepeat };

struct Extent3D {
    uint32_t width = 0;
    uint32_t height = 1;
    uint32_t depthOrArrayLayers = 1;
};
struct TexelCopyBufferLayout {
    uint64_t offset = 0;
    uint32_t bytesPerRow = 0;
    uint32_t rowsPerImage = 0;
};
struct TexelCopyTextureInfo {
    Texture texture;
    uint32_t mipLevel = 0;
};
struct VertexBufferLayout {};
struct SurfaceConfiguration {
    TextureFormat format = TextureFormat::Undefined;
    uint32_t width = 0;
    uint32_t height = 0;
};
struct SamplerDescriptor {
    AddressMode addressModeU = AddressMode::ClampToEdge;
    AddressMode addressModeV = AddressMode::ClampToEdge;
    AddressMode addressModeW = AddressMode::ClampToEdge;
    FilterMode magFilter = FilterMode::Nearest;
    FilterMode minFilter = FilterMode::Nearest;
    MipmapFilterMode mipmapFilter = MipmapFilterMode::Nearest;
    float lodMinClamp = 0.0f;
    float lodMaxClamp = 32.0f;
    uint16_t maxAnisotropy = 1;
};
struct Color {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 0.0;
};
using StringView = std::string_view;

} // namespace wgpu
