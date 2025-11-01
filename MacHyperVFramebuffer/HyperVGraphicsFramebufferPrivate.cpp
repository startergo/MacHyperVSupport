//
//  HyperVGraphicsFramebufferPrivate.cpp
//  Hyper-V synthetic graphics framebuffer driver
//
//  Copyright © 2025 Goldfish64. All rights reserved.
//

#include "HyperVGraphicsFramebuffer.hpp"

#define kHyperVSupportedResolutionsKey    "SupportedResolutions"
#define kHyperVHeightKey                  "Height"
#define kHyperVWidthKey                   "Width"

IOReturn HyperVGraphicsFramebuffer::initGraphicsService() {
  IOReturn status;

  if (_hvGfxProvider == nullptr) {
    return kIOReturnUnsupported;
  }

  //
  // Initialize graphics service and get version and graphics memory location.
  //
  status = _hvGfxProvider->callPlatformFunction(kHyperVGraphicsPlatformFunctionInit, true, &_gfxVersion, &_gfxBase, &_gfxLength, nullptr);
  if (status != kIOReturnSuccess) {
    return status;
  }

  HVDBGLOG("Graphics version %u.%u", _gfxVersion.major, _gfxVersion.minor);
  HVDBGLOG("Graphics memory located at %p length 0x%X", _gfxBase, _gfxLength);
  HVDBGLOG("Graphics bit depth: %u-bit", (_gfxVersion.value == kHyperVGraphicsVersionV3_0) ? kHyperVGraphicsBitDepth2008 : kHyperVGraphicsBitDepth);
  return kIOReturnSuccess;
}

IOReturn HyperVGraphicsFramebuffer::buildGraphicsModes() {
  OSArray *resArray = OSDynamicCast(OSArray, getProperty(kHyperVSupportedResolutionsKey));
  if (resArray == nullptr) {
    HVDBGLOG("No SupportedResolutions property, using dynamic mode generation");
    return buildDynamicModes();
  }

  //
  // Populate modes from Info.plist, filtering by VRAM availability.
  //
  UInt32 validModeCount = 0;
  UInt32 totalModeCount = resArray->getCount();
  HyperVGraphicsMode *tempModes = static_cast<HyperVGraphicsMode*>(IOMalloc(sizeof (*tempModes) * totalModeCount));
  if (tempModes == nullptr) {
    HVSYSLOG("Failed to allocate temporary graphics modes");
    return buildDynamicModes();
  }

  for (UInt32 i = 0; i < totalModeCount; i++) {
    //
    // Get height/width for each mode.
    //
    OSDictionary *modeDict = OSDynamicCast(OSDictionary, resArray->getObject(i));
    if (modeDict == nullptr) {
      HVSYSLOG("Graphics mode %u is not a dictionary", i);
      continue;
    }

    OSNumber *width = OSDynamicCast(OSNumber, modeDict->getObject(kHyperVWidthKey));
    OSNumber *height = OSDynamicCast(OSNumber, modeDict->getObject(kHyperVHeightKey));
    if ((width == nullptr) || (height == nullptr)) {
      HVSYSLOG("Graphics mode %u is missing keys", i);
      continue;
    }

    UInt32 modeWidth = width->unsigned32BitValue();
    UInt32 modeHeight = height->unsigned32BitValue();
    UInt32 requiredVRAM = modeWidth * modeHeight * (getScreenDepth() / kHyperVGraphicsBitsPerByte);

    //
    // Validate sizes are within range.
    //
    if (_gfxVersion.value == kHyperVGraphicsVersionV3_0) {
      if ((modeWidth > kHyperVGraphicsMaxWidth2008) || (modeHeight > kHyperVGraphicsMaxHeight2008)) {
        HVDBGLOG("Mode %ux%u exceeds v3.0 limits, skipping", modeWidth, modeHeight);
        continue;
      }
    }
    if ((modeWidth < kHyperVGraphicsMinWidth) || (modeHeight < kHyperVGraphicsMinHeight)) {
      HVDBGLOG("Mode %ux%u below minimum, skipping", modeWidth, modeHeight);
      continue;
    }
    if (requiredVRAM > _gfxLength) {
      HVDBGLOG("Mode %ux%u requires %u bytes, only %u available, skipping", modeWidth, modeHeight, requiredVRAM, _gfxLength);
      continue;
    }

    tempModes[validModeCount].width = modeWidth;
    tempModes[validModeCount].height = modeHeight;
    validModeCount++;
    HVDBGLOG("Added graphics mode %ux%u", modeWidth, modeHeight);
  }

  if (validModeCount == 0) {
    HVSYSLOG("No valid modes from Info.plist, falling back to dynamic generation");
    IOFree(tempModes, sizeof (*tempModes) * totalModeCount);
    return buildDynamicModes();
  }

  //
  // Allocate final mode array with only valid modes.
  //
  _gfxModesCount = validModeCount;
  _gfxModes = static_cast<HyperVGraphicsMode*>(IOMalloc(sizeof (*_gfxModes) * _gfxModesCount));
  if (_gfxModes == nullptr) {
    HVSYSLOG("Failed to allocate graphics modes");
    IOFree(tempModes, sizeof (*tempModes) * totalModeCount);
    return kIOReturnNoResources;
  }

  memcpy(_gfxModes, tempModes, sizeof (*_gfxModes) * _gfxModesCount);
  IOFree(tempModes, sizeof (*tempModes) * totalModeCount);

  HVDBGLOG("Loaded %u graphics modes from Info.plist", _gfxModesCount);
  return kIOReturnSuccess;
}

IOReturn HyperVGraphicsFramebuffer::buildDynamicModes() {
  //
  // Standard display resolutions to try.
  //
  struct { UInt32 width; UInt32 height; } standardModes[] = {
    {640, 480}, {800, 600}, {1024, 768}, {1152, 864},
    {1280, 720}, {1280, 1024}, {1366, 768}, {1440, 900},
    {1600, 900}, {1600, 1200}, {1680, 1050}, {1920, 1080},
    {1920, 1200}, {2560, 1440}, {3840, 2160}, {5120, 2880},
    {7680, 4320}
  };
  UInt32 maxModes = sizeof(standardModes) / sizeof(standardModes[0]);
  UInt32 validModeCount = 0;
  UInt32 maxWidth, maxHeight;

  //
  // Determine version-specific limits.
  //
  if (_gfxVersion.value == kHyperVGraphicsVersionV3_0) {
    maxWidth = kHyperVGraphicsMaxWidth2008;
    maxHeight = kHyperVGraphicsMaxHeight2008;
  } else if (_gfxVersion.value == kHyperVGraphicsVersionV3_2) {
    maxWidth = kHyperVGraphicsMaxWidth_V3_2;
    maxHeight = kHyperVGraphicsMaxHeight_V3_2;
  } else {
    maxWidth = kHyperVGraphicsMaxWidth_V3_5;
    maxHeight = kHyperVGraphicsMaxHeight_V3_5;
  }

  //
  // Count how many modes fit in available VRAM and version limits.
  //
  HyperVGraphicsMode *tempModes = static_cast<HyperVGraphicsMode*>(IOMalloc(sizeof (*tempModes) * maxModes));
  if (tempModes == nullptr) {
    HVSYSLOG("Failed to allocate temporary mode array");
    return buildFallbackMode();
  }

  for (UInt32 i = 0; i < maxModes; i++) {
    UInt32 width = standardModes[i].width;
    UInt32 height = standardModes[i].height;
    UInt32 requiredVRAM = width * height * (getScreenDepth() / kHyperVGraphicsBitsPerByte);

    if ((width <= maxWidth) && (height <= maxHeight) && (requiredVRAM <= _gfxLength)) {
      tempModes[validModeCount].width = width;
      tempModes[validModeCount].height = height;
      validModeCount++;
      HVDBGLOG("Added dynamic mode %ux%u", width, height);
    }
  }

  if (validModeCount == 0) {
    HVSYSLOG("No valid dynamic modes, using fallback");
    IOFree(tempModes, sizeof (*tempModes) * maxModes);
    return buildFallbackMode();
  }

  //
  // Allocate final mode array.
  //
  _gfxModesCount = validModeCount;
  _gfxModes = static_cast<HyperVGraphicsMode*>(IOMalloc(sizeof (*_gfxModes) * _gfxModesCount));
  if (_gfxModes == nullptr) {
    HVSYSLOG("Failed to allocate graphics modes");
    IOFree(tempModes, sizeof (*tempModes) * maxModes);
    return kIOReturnNoResources;
  }

  memcpy(_gfxModes, tempModes, sizeof (*_gfxModes) * _gfxModesCount);
  IOFree(tempModes, sizeof (*tempModes) * maxModes);

  HVDBGLOG("Generated %u dynamic graphics modes", _gfxModesCount);
  return kIOReturnSuccess;
}

IOReturn HyperVGraphicsFramebuffer::buildFallbackMode() {
  HVSYSLOG("Graphics modes could not be loaded, using fallback");

  //
  // Use default 1024x768 mode if the modes could not be fetched.
  //
  _gfxModesCount = 1;
  _gfxModes = static_cast<HyperVGraphicsMode*>(IOMalloc(sizeof (*_gfxModes) * _gfxModesCount));
  if (_gfxModes == nullptr) {
    HVSYSLOG("Failed to allocate graphics modes");
    return kIOReturnNoResources;
  }
  _gfxModes[0] = { 1024, 768 };
  return kIOReturnSuccess;
}
