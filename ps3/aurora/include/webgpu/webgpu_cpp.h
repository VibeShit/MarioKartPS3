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
    RGB10A2Unorm,
    R32Float,
    BC1RGBAUnorm,
    BC3RGBAUnorm,
    BC5RGUnorm,
    BC7RGBAUnorm,
    ASTC4x4Unorm, ASTC4x4UnormSrgb, ASTC5x4Unorm, ASTC5x4UnormSrgb, ASTC5x5Unorm, ASTC5x5UnormSrgb, ASTC6x5Unorm, ASTC6x5UnormSrgb, ASTC6x6Unorm, ASTC6x6UnormSrgb, ASTC8x5Unorm, ASTC8x5UnormSrgb, ASTC8x6Unorm, ASTC8x6UnormSrgb, ASTC8x8Unorm, ASTC8x8UnormSrgb, ASTC10x5Unorm, ASTC10x5UnormSrgb, ASTC10x6Unorm, ASTC10x6UnormSrgb, ASTC10x8Unorm, ASTC10x8UnormSrgb, ASTC10x10Unorm, ASTC10x10UnormSrgb, ASTC12x10Unorm, ASTC12x10UnormSrgb, ASTC12x12Unorm, ASTC12x12UnormSrgb,
    Depth24Plus,
    Depth32Float,
};
enum class LoadOp : uint32_t { Undefined, Load, Clear };
enum class StoreOp : uint32_t { Undefined, Store, Discard };
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
struct AdapterInfo {};
struct Limits {};
using StringView = std::string_view;

} // namespace wgpu
