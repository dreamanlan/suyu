// DDS file format structures and writer
// Add this to a header file (e.g., dds_writer.h)

#pragma once
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace DDSWriter {

// DDS pixel format flags
constexpr uint32_t DDPF_ALPHAPIXELS = 0x1;
constexpr uint32_t DDPF_ALPHA = 0x2;
constexpr uint32_t DDPF_FOURCC = 0x4;
constexpr uint32_t DDPF_RGB = 0x40;
constexpr uint32_t DDPF_RGBA = 0x41;
constexpr uint32_t DDPF_LUMINANCE = 0x20000;

// DDS header flags
constexpr uint32_t DDSD_CAPS = 0x1;
constexpr uint32_t DDSD_HEIGHT = 0x2;
constexpr uint32_t DDSD_WIDTH = 0x4;
constexpr uint32_t DDSD_PITCH = 0x8;
constexpr uint32_t DDSD_PIXELFORMAT = 0x1000;
constexpr uint32_t DDSD_MIPMAPCOUNT = 0x20000;
constexpr uint32_t DDSD_LINEARSIZE = 0x80000;
constexpr uint32_t DDSD_DEPTH = 0x800000;

// DDS caps flags
constexpr uint32_t DDSCAPS_COMPLEX = 0x8;
constexpr uint32_t DDSCAPS_TEXTURE = 0x1000;
constexpr uint32_t DDSCAPS_MIPMAP = 0x400000;

#pragma pack(push, 1)
struct DDSPixelFormat {
    uint32_t size = 32;
    uint32_t flags;
    uint32_t fourCC;
    uint32_t rgbBitCount = 0;
    uint32_t rBitMask = 0;
    uint32_t gBitMask = 0;
    uint32_t bBitMask = 0;
    uint32_t aBitMask = 0;
};

struct DDSHeader {
    uint32_t magic = 0x20534444; // "DDS "
    uint32_t size = 124;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitchOrLinearSize;
    uint32_t depth = 0;
    uint32_t mipMapCount = 1;
    uint32_t reserved1[11] = {};
    DDSPixelFormat pixelFormat;
    uint32_t caps;
    uint32_t caps2 = 0;
    uint32_t caps3 = 0;
    uint32_t caps4 = 0;
    uint32_t reserved2 = 0;
};

struct DDSHeaderDXT10 {
    uint32_t dxgiFormat;
    uint32_t resourceDimension = 3; // DDS_DIMENSION_TEXTURE2D
    uint32_t miscFlag = 0;
    uint32_t arraySize = 1;
    uint32_t miscFlags2 = 0;
};
#pragma pack(pop)

// Mipmap level data
struct MipmapData {
    uint32_t width;
    uint32_t height;
    const uint8_t* data;
    size_t size;
};

// Map PixelFormat to DDS format information
struct DDSFormatInfo {
    uint32_t fourCC;
    bool needDXT10;
    uint32_t dxgiFormat;
    uint32_t bytesPerBlock;
};

DDSFormatInfo GetDDSFormatInfo(VideoCore::Surface::PixelFormat format) {
    using VideoCore::Surface::PixelFormat;

    switch (format) {
    case PixelFormat::BC1_RGBA_UNORM:
        return {0x31545844, false, 0, 8}; // "DXT1", 8 bytes per block
    case PixelFormat::BC1_RGBA_SRGB:
        return {0x30315844, true, 72, 8}; // "DX10", DXGI_FORMAT_BC1_UNORM_SRGB
    case PixelFormat::BC2_UNORM:
        return {0x33545844, false, 0, 16}; // "DXT3", 16 bytes per block
    case PixelFormat::BC2_SRGB:
        return {0x30315844, true, 75, 16}; // "DX10", DXGI_FORMAT_BC2_UNORM_SRGB
    case PixelFormat::BC3_UNORM:
        return {0x35545844, false, 0, 16}; // "DXT5", 16 bytes per block
    case PixelFormat::BC3_SRGB:
        return {0x30315844, true, 78, 16}; // "DX10", DXGI_FORMAT_BC3_UNORM_SRGB
    case PixelFormat::BC4_UNORM:
        return {0x55344342, false, 0, 8}; // "BC4U", 8 bytes per block
    case PixelFormat::BC4_SNORM:
        return {0x53344342, false, 0, 8}; // "BC4S", 8 bytes per block
    case PixelFormat::BC5_UNORM:
        return {0x55354342, false, 0, 16}; // "BC5U", 16 bytes per block
    case PixelFormat::BC5_SNORM:
        return {0x53354342, false, 0, 16}; // "BC5S", 16 bytes per block
    case PixelFormat::BC6H_UFLOAT:
        return {0x30315844, true, 95, 16}; // "DX10", DXGI_FORMAT_BC6H_UF16
    case PixelFormat::BC6H_SFLOAT:
        return {0x30315844, true, 96, 16}; // "DX10", DXGI_FORMAT_BC6H_SF16
    case PixelFormat::BC7_UNORM:
        return {0x30315844, true, 98, 16}; // "DX10", DXGI_FORMAT_BC7_UNORM
    case PixelFormat::BC7_SRGB:
        return {0x30315844, true, 99, 16}; // "DX10", DXGI_FORMAT_BC7_UNORM_SRGB
    default:
        return {0x31545844, false, 0, 8}; // Default to DXT1
    }
}

inline bool WriteDDSLinearData(const std::string& filename, const uint8_t* linear_data, size_t data_size,
                               uint32_t width, uint32_t height, uint32_t pitchOrLinearSize,
                               const uint32_t num_layers, const uint32_t num_levels,
                               const DDSFormatInfo& formatInfo) {
    std::ofstream file(filename, std::ios::binary);
    if (!file) {
        LOG_ERROR(Render_Vulkan, "Failed to create DDS file: {}", filename);
        return false;
    }

    LOG_INFO(Render_Vulkan, "Writing DDS: fourCC={}, dxgiFormat={}, bytesPerBlock={}",
             formatInfo.fourCC, formatInfo.dxgiFormat, formatInfo.bytesPerBlock);

    // Write DDS header
    DDSHeader header{};
    header.flags =
        0x1 | 0x2 | 0x4 | 0x1000 | 0x80000; // CAPS | HEIGHT | WIDTH | PIXELFORMAT | LINEARSIZE
    header.height = height;
    header.width = width;
    header.pitchOrLinearSize = pitchOrLinearSize;
    header.caps = 0x1000; // DDSCAPS_TEXTURE

    // Add mipmap flags if needed
    if (num_levels > 1) {
        header.flags |= 0x20000; // MIPMAPCOUNT
        header.mipMapCount = num_levels;
        header.caps |= 0x8 | 0x400000; // COMPLEX | MIPMAP
    }

    // Setup pixel format
    header.pixelFormat.size = 32;
    header.pixelFormat.flags = 0x4; // DDPF_FOURCC
    header.pixelFormat.fourCC = formatInfo.fourCC;

    // Write DDS magic and header
    file.write(reinterpret_cast<const char*>(&header.magic), 4);
    file.write(reinterpret_cast<const char*>(&header.size), sizeof(DDSHeader) - 4);

    // Write DXT10 extended header if needed
    if (formatInfo.needDXT10) {
        DDSHeaderDXT10 header10{};
        header10.dxgiFormat = formatInfo.dxgiFormat;
        header10.resourceDimension = 3; // DDS_DIMENSION_TEXTURE2D
        header10.arraySize = num_layers;

        file.write(reinterpret_cast<const char*>(&header10), sizeof(DDSHeaderDXT10));
    }

    // Write deswizzled data
    file.write(reinterpret_cast<const char*>(linear_data), data_size);
    return true;
}

// Write DDS file with mipmaps and layers
// rgba_format: true for RGBA8, false for RGB8
// use_rgba16f: true for RGBA16F (BC6H decoded output)
// num_layers: number of array layers (default 1 for non-array textures)
inline bool WriteDDS(const std::string& filename, const std::vector<MipmapData>& mipmaps,
                     bool rgba_format = true, bool use_rgba16f = false, uint32_t num_layers = 1) {
    if (mipmaps.empty()) {
        return false;
    }

    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    // Calculate mip levels per layer
    // Total mipmaps = num_layers * mip_levels_per_layer
    const uint32_t mip_levels_per_layer = static_cast<uint32_t>(mipmaps.size()) / num_layers;

    // Setup DDS header
    DDSHeader header = {};
    header.magic = 0x20534444; // "DDS "
    header.size = 124; // Size of DDS_HEADER structure (excluding magic)
    header.flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_PITCH;
    header.height = mipmaps[0].height;
    header.width = mipmaps[0].width;

    // Calculate pitch based on format
    if (use_rgba16f) {
        header.pitchOrLinearSize = mipmaps[0].width * 8; // 8 bytes per pixel for RGBA16F
    } else {
        header.pitchOrLinearSize = mipmaps[0].width * (rgba_format ? 4 : 3);
    }

    header.depth = 0;
    header.mipMapCount = mip_levels_per_layer;

    if (mip_levels_per_layer > 1) {
        header.flags |= DDSD_MIPMAPCOUNT;
        header.caps |= DDSCAPS_COMPLEX | DDSCAPS_MIPMAP;
    }

    header.caps |= DDSCAPS_TEXTURE;

    // Setup pixel format
    header.pixelFormat.size = sizeof(DDSPixelFormat);

    // Use DXT10 extended header for RGBA16F or multi-layer textures
    const bool need_dxt10 = use_rgba16f || num_layers > 1;

    if (need_dxt10) {
        // Use DX10 extended header
        header.pixelFormat.flags = DDPF_FOURCC;
        header.pixelFormat.fourCC = 0x30315844; // "DX10"
    } else if (rgba_format) {
        header.pixelFormat.flags = DDPF_RGB | DDPF_ALPHAPIXELS;
        header.pixelFormat.rgbBitCount = 32;
        header.pixelFormat.rBitMask = 0x000000FF;
        header.pixelFormat.gBitMask = 0x0000FF00;
        header.pixelFormat.bBitMask = 0x00FF0000;
        header.pixelFormat.aBitMask = 0xFF000000;
    } else {
        header.pixelFormat.flags = DDPF_RGB;
        header.pixelFormat.rgbBitCount = 24;
        header.pixelFormat.rBitMask = 0x00FF0000;
        header.pixelFormat.gBitMask = 0x0000FF00;
        header.pixelFormat.bBitMask = 0x000000FF;
        header.pixelFormat.aBitMask = 0x00000000;
    }

    // Write header (magic + header structure)
    file.write(reinterpret_cast<const char*>(&header.magic), sizeof(uint32_t)); // Write magic
    file.write(reinterpret_cast<const char*>(&header.size), sizeof(DDSHeader) - sizeof(uint32_t)); // Write rest of header

    // Write DXT10 extended header if needed
    if (need_dxt10) {
        DDSHeaderDXT10 header10{};

        // Set DXGI format based on pixel format
        if (use_rgba16f) {
            header10.dxgiFormat = 10; // DXGI_FORMAT_R16G16B16A16_FLOAT
        } else if (rgba_format) {
            header10.dxgiFormat = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
        } else {
            // RGB8 doesn't have a direct DXGI format, use RGBA8 and ignore alpha
            header10.dxgiFormat = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
        }

        header10.resourceDimension = 3; // DDS_DIMENSION_TEXTURE2D
        header10.arraySize = num_layers;
        header10.miscFlag = 0;
        header10.miscFlags2 = 0;

        file.write(reinterpret_cast<const char*>(&header10), sizeof(DDSHeaderDXT10));
    }

    // Write mipmap data
    for (const auto& mip : mipmaps) {
        file.write(reinterpret_cast<const char*>(mip.data), mip.size);
    }

    file.close();
    return true;
}

// Write single level DDS (for convenience)
inline bool WriteDDSSingleLevel(const std::string& filename, uint32_t width, uint32_t height,
                                const uint8_t* data, bool rgba_format = true) {
    MipmapData mip;
    mip.width = width;
    mip.height = height;
    mip.data = data;
    mip.size = width * height * (rgba_format ? 4 : 3);

    std::vector<MipmapData> mipmaps = {mip};
    return WriteDDS(filename, mipmaps, rgba_format);
}

} // namespace DDSWriter