# Texture Format Compatibility Solution for macOS

## Problem Overview

Switch emulator on Windows supports various texture compression formats natively, but the macOS version (using MoltenVK/Metal) often encounters unsupported texture format issues.

### Root Cause

- **MoltenVK Limitation**: MoltenVK translates Vulkan to Metal, but Metal doesn't natively support certain Vulkan texture formats:
  - **BCn compression formats** (BC1-BC7): Not supported by Metal
  - **Integer formats** (SINT/UINT): Limited support in Metal
  - **ASTC formats**: Supported by Metal but MoltenVK implementation may be incomplete

## Solution Strategy

This implementation uses a **two-tier fallback approach**:

### Method 1: Hardware Format Fallback (Primary)
- Automatically detect unsupported formats at runtime
- Convert to compatible formats that Metal supports
- Minimal performance impact
- Maintains good visual quality

### Method 2: Software Transcoding (Fallback)
- When hardware fallback fails, automatically use CPU-based transcoding
- Decode compressed textures on CPU
- Higher CPU overhead but ensures 100% compatibility
- Lossless quality

## Implementation Details

### 1. Enhanced Format Detection (`vulkan_device.cpp`)

```cpp
// Force disable BCn support on MoltenVK
if (is_mvk) {
    LOG_INFO(Render_Vulkan, "MoltenVK detected: BCn texture compression not supported");
    features.features.textureCompressionBC = VK_FALSE;
}
```

**What it does:**
- Detects MoltenVK driver at initialization
- Forces BCn support flag to false
- Triggers automatic format fallback system

### 2. Improved Format Conversion (`maxwell_to_vk.cpp`)

**ASTC Format Handling:**
- Uncompressed mode: ASTC → RGBA8 (best compatibility)
- BC1 mode: ASTC → BC1 (if BCn supported)
- BC3 mode: ASTC → BC3 (if BCn supported)

**BCn Format Handling:**
- BC1/BC2/BC3 → RGBA8 (with software decode)
- BC4 → R8 (with software decode)
- BC5 → RG8 (with software decode)
- BC6H → RGBA16F (with software decode)
- BC7 → RGBA8 (with software decode)

### 3. Integer Format Conversion (`surface.cpp`)

On Apple platforms, integer formats are automatically converted to float:
- R8_UINT/SINT → R16_FLOAT
- R16_UINT/SINT → R16_FLOAT
- R32_UINT/SINT → R32_FLOAT
- RGBA8_UINT/SINT → RGBA16_FLOAT

### 4. Software Transcoding (`decode_bc.cpp`)

When hardware fallback isn't sufficient:
- Uses `bc_decoder` library for BCn formats
- Uses compute shader for ASTC formats
- Automatic activation when needed
- Transparent to the user

## Performance Characteristics

| Method | Memory Usage | CPU Overhead | GPU Overhead | Quality |
|--------|--------------|--------------|--------------|---------|
| Native Support | Low | None | Low | Perfect |
| Hardware Fallback | Medium | Low | Medium | Excellent |
| Software Transcoding | Low | High | Low | Perfect |

## User Configuration

### Recommended Settings for macOS

1. **ASTC Recompression**: Set to "Uncompressed"
   - Path: Settings → Graphics → Advanced
   - Provides best compatibility on Mac
   - Higher memory usage but no quality loss

2. **Update System**:
   - Keep macOS updated for latest Metal improvements
   - Update Xcode for latest MoltenVK features

## Technical Flow

```
Game Texture Request
        ↓
Format Detection
        ↓
    Supported? ──Yes──→ Use Native Format
        ↓ No
        ↓
Method 1: Hardware Fallback
        ↓
Convert to Compatible Format
        ↓
    Success? ──Yes──→ Use Fallback Format
        ↓ No
        ↓
Method 2: Software Transcoding
        ↓
CPU Decode to Uncompressed
        ↓
Upload to GPU
        ↓
Render
```

## Logging

The system provides detailed logging to help diagnose format issues:

```
[Info] MoltenVK detected: BCn texture compression not supported by Metal
[Debug] BCn format VK_FORMAT_BC3_UNORM_BLOCK fallback to A8B8G8R8_UNORM (software decode will be used)
[Debug] ASTC format VK_FORMAT_ASTC_4x4_UNORM_BLOCK fallback to VK_FORMAT_A8B8G8R8_UNORM_PACK32 (Uncompressed RGBA8)
```

## Supported Formats

### ✅ Fully Supported on macOS
- RGBA8/16/32 (all variants)
- RGB565, RGB5A1
- Depth/Stencil formats
- Float formats (R16F, R32F, RGBA16F, RGBA32F)
- ASTC (via fallback/transcoding)

### ⚠️ Supported via Fallback
- BCn formats (BC1-BC7) → Software transcoding
- Integer formats → Float conversion
- ASTC → RGBA8 or BC1/BC3

### ❌ Not Supported
- None (all formats have fallback paths)

## Troubleshooting

### Issue: Textures appear corrupted
**Solution**: Set ASTC Recompression to "Uncompressed" in settings

### Issue: Performance degradation
**Cause**: Software transcoding active
**Solution**:
- Expected behavior on Mac
- Consider using games with fewer compressed textures
- Upgrade to Apple Silicon for better performance

### Issue: High memory usage
**Cause**: Uncompressed format fallback
**Trade-off**: Memory vs Compatibility
**Note**: This is necessary for proper rendering on Mac

## Future Improvements

1. **GPU-based BCn Decoding**: Implement compute shader BCn decoder (like ASTC)
2. **Format Cache**: Cache transcoded textures to reduce CPU overhead
3. **Adaptive Quality**: Automatically adjust quality based on system performance
4. **MoltenVK Updates**: Monitor MoltenVK for native BCn support

## References

- [MoltenVK Documentation](https://github.com/KhronosGroup/MoltenVK)
- [Metal Feature Set Tables](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf)
- [Vulkan Format Compatibility](https://www.khronos.org/registry/vulkan/specs/1.3/html/vkspec.html#formats)

## Credits

This solution implements a robust two-tier fallback system that prioritizes hardware acceleration while ensuring 100% format compatibility through software transcoding when necessary.
