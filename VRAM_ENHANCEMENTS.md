# VRAM Enhancement Implementation

## Overview
Enhanced the Hyper-V graphics driver and framebuffer to support higher VRAM configurations and higher display resolutions up to 4K and beyond.

## Changes Implemented

### 1. HyperVGraphicsRegs.hpp
**Added version-specific resolution constants:**
- `kHyperVGraphicsMaxWidth_V3_2` / `kHyperVGraphicsMaxHeight_V3_2` (3840x2160 for Windows 8+)
- `kHyperVGraphicsMaxWidth_V3_5` / `kHyperVGraphicsMaxHeight_V3_5` (7680x4320 for Windows 10 v1809+)

### 2. HyperVGraphics.hpp
**Added field:**
- `_gfxBaseAllocated` - Tracks whether MMIO range was dynamically allocated (for cleanup)

### 3. HyperVGraphicsPrivate.cpp
**Enhanced `allocateGraphicsMemory()`:**
- Removed hard-coded `0xF8000000` address
- Implemented dynamic MMIO allocation via `HyperVPCIRoot::allocateRange()`
- Added support for `VRAMSizeBytes` property to override VRAM size
- Enhanced logging to show VRAM size in MB

**Enhanced `setScreenResolutionGated()`:**
- Added detailed VRAM requirement calculation
- Version-specific resolution validation (v3.0, v3.2, v3.5)
- Improved error messages showing required vs available VRAM
- Returns `kIOReturnNoMemory` when VRAM is insufficient

### 4. HyperVGraphics.cpp
**Enhanced `stop()` method:**
- Added proper MMIO range deallocation via `HyperVPCIRoot::deallocateRange()`
- Prevents memory leaks when driver is unloaded

### 5. HyperVGraphicsFramebuffer.hpp
**Added function declaration:**
- `buildDynamicModes()` - Generates display modes based on available VRAM

### 6. HyperVGraphicsFramebufferPrivate.cpp
**Enhanced `buildGraphicsModes()`:**
- Now filters Info.plist modes by VRAM availability
- Falls back to dynamic generation if no valid modes in plist
- Better error handling and logging

**New `buildDynamicModes()` function:**
- Auto-generates standard resolution modes (640x480 up to 7680x4320)
- Filters modes based on:
  - Graphics protocol version limits
  - Available VRAM capacity
  - Minimum resolution requirements
- Supports 17 standard resolutions including:
  - Common: 1080p, 1440p, 4K
  - Ultra-wide: 1680x1050, 1920x1200
  - High-end: 5K (5120x2880), 8K (7680x4320)

### 7. Info.plist
**Added higher resolution modes:**
- 1366x768 (HD)
- 1440x900 (WXGA+)
- 1600x900 (HD+)
- 1680x1050 (WSXGA+)
- 1920x1080 (Full HD)
- 1920x1200 (WUXGA)
- 2560x1440 (QHD)
- 3840x2160 (4K UHD)

## Benefits

### 1. Dynamic VRAM Management
- No longer restricted to hard-coded memory address
- Properly allocates and deallocates MMIO ranges
- Reduces conflicts with other devices

### 2. Higher Resolution Support
- Native support for modern displays (1080p, 1440p, 4K)
- Automatic mode filtering based on available VRAM
- Version-aware resolution limits

### 3. Better Error Handling
- Detailed logging of VRAM usage
- Clear error messages when resolution exceeds capacity
- Graceful fallback to supported modes

### 4. Manual Override Capability
- Can set custom VRAM size via `VRAMSizeBytes` property
- Useful for testing or special configurations

## Usage

### Default Operation
The driver will automatically:
1. Query VRAM size from VMBus channel
2. Dynamically allocate MMIO range
3. Generate or filter display modes based on available VRAM
4. Select the highest supported resolution

### Manual VRAM Override
Add to Info.plist or boot-args:
```xml
<key>VRAMSizeBytes</key>
<integer>67108864</integer>  <!-- 64 MB -->
```

### Host Configuration
To increase VRAM on Hyper-V host:
```powershell
# Set display resolution which affects VRAM allocation
Set-VMVideo -VMName "MacVM" -HorizontalResolution 3840 -VerticalResolution 2160

# Or configure in Hyper-V Manager:
# VM Settings > Display > Maximum Resolution
```

## Testing Recommendations

1. **Test with different VRAM sizes:**
   - 16 MB (minimal)
   - 32 MB (1080p at 32-bit)
   - 64 MB (1440p at 32-bit)
   - 128 MB (4K at 32-bit)

2. **Test on different Hyper-V versions:**
   - Windows Server 2008 R2 (v3.0) - Should limit to 1600x1200@16-bit
   - Windows 8/Server 2012+ (v3.2) - Should support up to 4K
   - Windows 10 v1809+ (v3.5) - Should support up to 8K

3. **Test mode switching:**
   - Verify all modes are displayed correctly
   - Check that invalid modes are filtered
   - Confirm smooth resolution changes

4. **Test cleanup:**
   - Verify MMIO range is properly deallocated on driver unload
   - Check for memory leaks

## Potential Future Enhancements

1. **Dynamic VRAM scaling:**
   - Request more VRAM from host if available
   - Negotiate optimal VRAM size based on desired resolution

2. **Multi-monitor support:**
   - Allocate VRAM per display
   - Manage multiple framebuffers

3. **Compression:**
   - Implement framebuffer compression to reduce VRAM requirements
   - Support for higher resolutions with limited VRAM

4. **Performance monitoring:**
   - Track VRAM usage statistics
   - Log resolution change performance

## Notes

- The ultimate VRAM limit is still controlled by the Hyper-V host via `mmioSizeMegabytes` in the VMBus channel offer
- Driver gracefully handles whatever VRAM size the host provides
- All changes maintain backward compatibility with older Hyper-V versions
- Dynamic mode generation ensures at least one valid mode is always available
