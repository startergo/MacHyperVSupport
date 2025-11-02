//
//  HyperVGraphicsPrivate.cpp
//  Hyper-V synthetic graphics driver
//
//  Copyright © 2025 Goldfish64. All rights reserved.
//

#include "HyperVGraphics.hpp"
#include "HyperVPCIRoot.hpp"
#include "HyperVPlatformProvider.hpp"

//
// Dirty rectangle tracking implementation.
//
void HyperVGraphics::initDirtyTracking() {
  if (_screenWidth == 0 || _screenHeight == 0) {
    return;
  }

  // Calculate number of tiles needed
  _dirtyTilesX = (_screenWidth + kDirtyTileSize - 1) / kDirtyTileSize;
  _dirtyTilesY = (_screenHeight + kDirtyTileSize - 1) / kDirtyTileSize;
  _dirtyBitmapSize = ((_dirtyTilesX * _dirtyTilesY) + 7) / 8;  // Bits to bytes

  // Allocate bitmap
  _dirtyBitmap = static_cast<UInt8*>(IOMalloc(_dirtyBitmapSize));
  if (_dirtyBitmap != nullptr) {
    markFullScreenDirty();
    HVDBGLOG("Initialized dirty tracking: %ux%u tiles, %u bytes", _dirtyTilesX, _dirtyTilesY, _dirtyBitmapSize);
  } else {
    HVDBGLOG("Failed to allocate dirty bitmap");
  }
}

void HyperVGraphics::cleanupDirtyTracking() {
  if (_dirtyBitmap != nullptr) {
    IOFree(_dirtyBitmap, _dirtyBitmapSize);
    _dirtyBitmap = nullptr;
  }
  _dirtyTilesX = 0;
  _dirtyTilesY = 0;
  _dirtyBitmapSize = 0;
}

void HyperVGraphics::markFullScreenDirty() {
  _fullScreenDirty = true;
  if (_dirtyBitmap != nullptr) {
    memset(_dirtyBitmap, 0xFF, _dirtyBitmapSize);
  }
}

void HyperVGraphics::markRegionDirty(UInt32 x, UInt32 y, UInt32 width, UInt32 height) {
  if (_dirtyBitmap == nullptr) {
    _fullScreenDirty = true;
    return;
  }

  // Convert pixel coordinates to tile coordinates
  UInt32 startTileX = x / kDirtyTileSize;
  UInt32 startTileY = y / kDirtyTileSize;
  UInt32 endTileX = (x + width + kDirtyTileSize - 1) / kDirtyTileSize;
  UInt32 endTileY = (y + height + kDirtyTileSize - 1) / kDirtyTileSize;

  // Clamp to valid range
  if (endTileX > _dirtyTilesX) endTileX = _dirtyTilesX;
  if (endTileY > _dirtyTilesY) endTileY = _dirtyTilesY;

  // Mark tiles as dirty
  for (UInt32 ty = startTileY; ty < endTileY; ty++) {
    for (UInt32 tx = startTileX; tx < endTileX; tx++) {
      UInt32 bitIndex = ty * _dirtyTilesX + tx;
      _dirtyBitmap[bitIndex / 8] |= (1 << (bitIndex % 8));
    }
  }
}

bool HyperVGraphics::isDirty() {
  if (_fullScreenDirty) {
    return true;
  }
  if (_dirtyBitmap == nullptr) {
    return true;  // No tracking, assume dirty
  }

  // Check if any bit is set
  for (UInt32 i = 0; i < _dirtyBitmapSize; i++) {
    if (_dirtyBitmap[i] != 0) {
      return true;
    }
  }
  return false;
}

UInt32 HyperVGraphics::buildDirtyRectangles(HyperVGraphicsImageUpdateRectangle *rects, UInt32 maxRects) {
  if (_fullScreenDirty || _dirtyBitmap == nullptr) {
    // Full screen update
    rects[0].x1 = 0;
    rects[0].y1 = 0;
    rects[0].x2 = _screenWidth;
    rects[0].y2 = _screenHeight;
    return 1;
  }

  // Build rectangles from dirty tiles
  // For simplicity, we'll use a scanline approach to merge adjacent dirty tiles
  UInt32 rectCount = 0;

  for (UInt32 ty = 0; ty < _dirtyTilesY && rectCount < maxRects; ty++) {
    UInt32 startX = 0xFFFFFFFF;
    
    for (UInt32 tx = 0; tx <= _dirtyTilesX; tx++) {
      bool isDirtyTile = false;
      
      if (tx < _dirtyTilesX) {
        UInt32 bitIndex = ty * _dirtyTilesX + tx;
        isDirtyTile = (_dirtyBitmap[bitIndex / 8] & (1 << (bitIndex % 8))) != 0;
      }

      if (isDirtyTile && startX == 0xFFFFFFFF) {
        // Start of dirty region
        startX = tx;
      } else if (!isDirtyTile && startX != 0xFFFFFFFF) {
        // End of dirty region, create rectangle
        rects[rectCount].x1 = startX * kDirtyTileSize;
        rects[rectCount].y1 = ty * kDirtyTileSize;
        rects[rectCount].x2 = tx * kDirtyTileSize;
        rects[rectCount].y2 = (ty + 1) * kDirtyTileSize;
        
        // Clamp to screen bounds
        if (rects[rectCount].x2 > _screenWidth) rects[rectCount].x2 = _screenWidth;
        if (rects[rectCount].y2 > _screenHeight) rects[rectCount].y2 = _screenHeight;
        
        rectCount++;
        startX = 0xFFFFFFFF;
        
        if (rectCount >= maxRects) {
          break;
        }
      }
    }
  }

  return rectCount > 0 ? rectCount : 1;  // Return at least 1 (will be full screen)
}

void HyperVGraphics::clearDirtyFlags() {
  _fullScreenDirty = false;
  if (_dirtyBitmap != nullptr) {
    memset(_dirtyBitmap, 0, _dirtyBitmapSize);
  }
}

void HyperVGraphics::handleRefreshTimer(IOTimerEventSource *sender) {
  if (_fbReady) {
    refreshFramebufferImage();
  }
  _timerEventSource->setTimeoutMS(kHyperVGraphicsImageUpdateRefreshRateMS);
}

void HyperVGraphics::handlePacket(VMBusPacketHeader *pktHeader, UInt32 pktHeaderLength, UInt8 *pktData, UInt32 pktDataLength) {
  HyperVGraphicsMessage *gfxMsg = (HyperVGraphicsMessage*) pktData;
  void                  *responseBuffer;
  UInt32                responseLength;

  if (gfxMsg->pipeHeader.type != kHyperVGraphicsPipeMessageTypeData || gfxMsg->pipeHeader.size < __offsetof (HyperVGraphicsMessage, gfxHeader.size)) {
    HVDBGLOG("Invalid pipe packet receieved (type 0x%X, size %u)", gfxMsg->pipeHeader.type, gfxMsg->pipeHeader.size);
    return;
  }

  HVDBGLOG("Received packet type 0x%X (%u bytes)", gfxMsg->gfxHeader.type, gfxMsg->gfxHeader.size);
  switch (gfxMsg->gfxHeader.type) {
    case kHyperVGraphicsMessageTypeVersionResponse:
    case kHyperVGraphicsMessageTypeVRAMAck:
    case kHyperVGraphicsMessageTypeResolutionUpdateAck:
      if (_hvDevice->getPendingTransaction(kHyperVGraphicsRequestTransactionBaseID + gfxMsg->gfxHeader.type,
                                           &responseBuffer, &responseLength)) {
        memcpy(responseBuffer, pktData, responseLength);
        _hvDevice->wakeTransaction(kHyperVGraphicsRequestTransactionBaseID + gfxMsg->gfxHeader.type);
      }
      break;
      
    case kHyperVGraphicsMessageTypeFeatureChange:
      //
      // Refresh display states on a feature change.
      //
      HVDBGLOG("Got feature change: img %u pos %u shape %u res %u", gfxMsg->featureUpdate.isImageUpdateNeeded, gfxMsg->featureUpdate.isCursorPositionNeeded,
               gfxMsg->featureUpdate.isCursorShapeNeeded, gfxMsg->featureUpdate.isResolutionUpdateNeeded);
      if (_fbReady) {
        if (gfxMsg->featureUpdate.isResolutionUpdateNeeded) {
          setScreenResolution(_screenWidth, _screenHeight, false);
        }
        if (gfxMsg->featureUpdate.isImageUpdateNeeded) {
          markFullScreenDirty();
          refreshFramebufferImage();
        }
        if (gfxMsg->featureUpdate.isCursorShapeNeeded) {
          setCursorShape(nullptr, true);
        }
        if (gfxMsg->featureUpdate.isCursorPositionNeeded) {
          setCursorPosition(0, 0, false, true);
        }
      } else {
        HVDBGLOG("Ignoring feature change, not ready");
      }
      break;

    default:
      break;
  }
}

IOReturn HyperVGraphics::sendGraphicsMessage(HyperVGraphicsMessage *gfxMessage, HyperVGraphicsMessageType responseType,
                                             HyperVGraphicsMessage *gfxMessageResponse) {
  gfxMessage->pipeHeader.type = kHyperVGraphicsPipeMessageTypeData;
  gfxMessage->pipeHeader.size = gfxMessage->gfxHeader.size;

  return _hvDevice->writeInbandPacketWithTransactionId(gfxMessage, gfxMessage->gfxHeader.size + sizeof (gfxMessage->pipeHeader),
                                                       kHyperVGraphicsRequestTransactionBaseID + responseType,
                                                       gfxMessageResponse != nullptr, gfxMessageResponse,
                                                       gfxMessageResponse != nullptr ? sizeof(*gfxMessageResponse) : 0);
}

IOReturn HyperVGraphics::negotiateVersion(VMBusVersion version) {
  IOReturn status;
  HyperVGraphicsMessage gfxMsg = { };

  gfxMsg.gfxHeader.type = kHyperVGraphicsMessageTypeVersionRequest;
  gfxMsg.gfxHeader.size = sizeof (gfxMsg.gfxHeader) + sizeof (gfxMsg.versionRequest);
  gfxMsg.versionRequest.version = version;

  HVDBGLOG("Trying version %u.%u", version.major, version.minor);
  status = sendGraphicsMessage(&gfxMsg, kHyperVGraphicsMessageTypeVersionResponse, &gfxMsg);
  if (status != kIOReturnSuccess) {
    HVSYSLOG("Failed to send negotiate version with status 0x%X", status);
    return status;
  }

  HVDBGLOG("Version %u.%u accepted: 0x%X (actual version %u.%u) max video outputs: %u", version.major, version.minor,
           gfxMsg.versionResponse.accepted, gfxMsg.versionResponse.version.major,
           gfxMsg.versionResponse.version.minor, gfxMsg.versionResponse.maxVideoOutputs);
  return gfxMsg.versionResponse.accepted != 0 ? kIOReturnSuccess : kIOReturnUnsupported;
}

IOReturn HyperVGraphics::allocateGraphicsMemory(IOPhysicalAddress *outBase, UInt32 *outLength) {
  OSNumber *mmioBytesNumber;
  OSNumber *overrideVRAM;

  //
  // Hyper-V reserves 0xF8000000 specifically for synthetic video device.
  // This address is safe because:
  // 1. Hyper-V explicitly reserves it for synthvid in ACPI tables
  // 2. Won't conflict with DDA (Discrete Device Assignment) device BARs
  // 3. DDA devices get pre-assigned physical addresses that avoid this range
  // 4. The PCI allocator can't disambiguate DDA vs. available MMIO space,
  //    so using the known-safe reserved address avoids potential conflicts.
  //
  const IOPhysicalAddress kHyperVSyntheticVideoReservedBase = 0xF8000000;

  //
  // Check for manual VRAM size override via property.
  //
  overrideVRAM = OSDynamicCast(OSNumber, getProperty("VRAMSizeBytes"));
  if (overrideVRAM != nullptr) {
    *outLength = static_cast<UInt32>(overrideVRAM->unsigned64BitValue());
    HVDBGLOG("Using override VRAM size: 0x%X bytes (%u MB)", *outLength, *outLength / (1024 * 1024));
  } else {
    //
    // Get MMIO bytes from VMBus channel.
    //
    mmioBytesNumber = OSDynamicCast(OSNumber, _hvDevice->getProperty(kHyperVVMBusDeviceChannelMMIOByteCount));
    if (mmioBytesNumber == nullptr) {
      HVSYSLOG("Failed to get MMIO byte count");
      return kIOReturnNoResources;
    }
    *outLength = static_cast<UInt32>(mmioBytesNumber->unsigned64BitValue());
  }

  //
  // Use the Hyper-V reserved address for synthetic video.
  // This is the safest approach for both synthetic-only and DDA configurations.
  //
  *outBase = kHyperVSyntheticVideoReservedBase;
  _gfxBaseAllocated = false;  // Not dynamically allocated, so don't free in stop()

  HVDBGLOG("Graphics memory using Hyper-V reserved address %p length 0x%X (%u MB)", 
           *outBase, *outLength, *outLength / (1024 * 1024));
  return kIOReturnSuccess;
}

IOReturn HyperVGraphics::refreshFramebufferImage() {
  HyperVGraphicsMessage gfxMsg = { };
  IOReturn              status;

  //
  // Check if there are any dirty regions to update.
  //
  if (!isDirty()) {
    return kIOReturnSuccess;  // Nothing to update
  }

  //
  // Build dirty rectangles for update.
  //
  UInt32 rectCount = buildDirtyRectangles(&gfxMsg.imageUpdate.rects[0], 1);
  
  //
  // Send screen image update to Hyper-V with dirty regions.
  //
  gfxMsg.gfxHeader.type = kHyperVGraphicsMessageTypeImageUpdate;
  gfxMsg.gfxHeader.size = sizeof (gfxMsg.gfxHeader) + sizeof (UInt8) * 2 + 
                          sizeof (HyperVGraphicsImageUpdateRectangle) * rectCount;

  gfxMsg.imageUpdate.videoOutput = 0;
  gfxMsg.imageUpdate.count       = rectCount;

  status = sendGraphicsMessage(&gfxMsg);
  if (status != kIOReturnSuccess) {
    HVSYSLOG("Failed to send image update with status 0x%X", status);
  } else {
    // Clear dirty flags after successful update
    clearDirtyFlags();
  }
  return status;
}

IOReturn HyperVGraphics::setGraphicsMemory(IOPhysicalAddress base, UInt32 length) {
  IOReturn status;
  HyperVGraphicsMessage gfxMsg = { };

  //
  // Set location of graphics memory (VRAM).
  //
  gfxMsg.gfxHeader.type = kHyperVGraphicsMessageTypeVRAMLocation;
  gfxMsg.gfxHeader.size = sizeof (gfxMsg.gfxHeader) + sizeof (gfxMsg.vramLocation);

  gfxMsg.vramLocation.context            = gfxMsg.vramLocation.vramGPA = base;
  gfxMsg.vramLocation.isVRAMGPASpecified = 1;

  status = sendGraphicsMessage(&gfxMsg, kHyperVGraphicsMessageTypeVRAMAck, &gfxMsg);
  if (status != kIOReturnSuccess) {
    HVSYSLOG("Failed to send graphics memory location with status 0x%X", status);
    return status;
  }
  if (gfxMsg.vramAck.context != base) {
    HVSYSLOG("Returned context 0x%llX is incorrect, should be %p", gfxMsg.vramAck.context, base);
    return kIOReturnIOError;
  }

  HVDBGLOG("Set graphics memory location to %p length 0x%X", base, length);
  return kIOReturnSuccess;
}

IOReturn HyperVGraphics::setScreenResolution(UInt32 width, UInt32 height, bool waitForAck) {
  return _cmdGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &HyperVGraphics::setScreenResolutionGated), &width, &height, &waitForAck);
}

IOReturn HyperVGraphics::setScreenResolutionGated(UInt32 *width, UInt32 *height, bool *waitForAck) {
  HyperVGraphicsMessage gfxMsg = { };
  IOReturn              status;
  UInt32                requiredVRAM;

  //
  // Calculate required VRAM for this resolution.
  //
  requiredVRAM = (*width) * (*height) * (getScreenDepth() / kHyperVGraphicsBitsPerByte);

  //
  // Check version-specific bounds.
  //
  if (_gfxVersion.value == kHyperVGraphicsVersionV3_0) {
    if ((*width > kHyperVGraphicsMaxWidth2008) || (*height > kHyperVGraphicsMaxHeight2008)) {
      HVSYSLOG("Resolution %ux%u exceeds v3.0 maximum (%ux%u)", *width, *height,
               kHyperVGraphicsMaxWidth2008, kHyperVGraphicsMaxHeight2008);
      return kIOReturnBadArgument;
    }
  } else if (_gfxVersion.value == kHyperVGraphicsVersionV3_2) {
    if ((*width > kHyperVGraphicsMaxWidth_V3_2) || (*height > kHyperVGraphicsMaxHeight_V3_2)) {
      HVSYSLOG("Resolution %ux%u exceeds v3.2 maximum (%ux%u)", *width, *height,
               kHyperVGraphicsMaxWidth_V3_2, kHyperVGraphicsMaxHeight_V3_2);
      return kIOReturnBadArgument;
    }
  } else if (_gfxVersion.value == kHyperVGraphicsVersionV3_5) {
    if ((*width > kHyperVGraphicsMaxWidth_V3_5) || (*height > kHyperVGraphicsMaxHeight_V3_5)) {
      HVSYSLOG("Resolution %ux%u exceeds v3.5 maximum (%ux%u)", *width, *height,
               kHyperVGraphicsMaxWidth_V3_5, kHyperVGraphicsMaxHeight_V3_5);
      return kIOReturnBadArgument;
    }
  }

  //
  // Check minimum bounds.
  //
  if ((*width < kHyperVGraphicsMinWidth) || (*height < kHyperVGraphicsMinHeight)) {
    HVSYSLOG("Resolution %ux%u below minimum (%ux%u)", *width, *height,
             kHyperVGraphicsMinWidth, kHyperVGraphicsMinHeight);
    return kIOReturnBadArgument;
  }

  //
  // Check VRAM availability.
  //
  if (requiredVRAM > _gfxLength) {
    HVSYSLOG("Resolution %ux%ux%u requires %u bytes (%u MB), only %u bytes (%u MB) available",
             *width, *height, getScreenDepth(), requiredVRAM, requiredVRAM / (1024 * 1024),
             _gfxLength, _gfxLength / (1024 * 1024));
    return kIOReturnNoMemory;
  }

  //
  // Set screen resolution and pixel depth information.
  //
  HVDBGLOG("Setting screen resolution to %ux%ux%u", *width, *height, getScreenDepth());
  gfxMsg.gfxHeader.type = kHyperVGraphicsMessageTypeResolutionUpdate;
  gfxMsg.gfxHeader.size = sizeof (gfxMsg.gfxHeader) + sizeof (gfxMsg.resolutionUpdate);

  gfxMsg.resolutionUpdate.context                    = 0;
  gfxMsg.resolutionUpdate.videoOutputCount           = 1;
  gfxMsg.resolutionUpdate.videoOutputs[0].active     = 1;
  gfxMsg.resolutionUpdate.videoOutputs[0].vramOffset = 0;
  gfxMsg.resolutionUpdate.videoOutputs[0].depth      = getScreenDepth();
  gfxMsg.resolutionUpdate.videoOutputs[0].width      = *width;
  gfxMsg.resolutionUpdate.videoOutputs[0].height     = *height;
  gfxMsg.resolutionUpdate.videoOutputs[0].pitch      = *width * (getScreenDepth() / kHyperVGraphicsBitsPerByte);

  status = sendGraphicsMessage(&gfxMsg, kHyperVGraphicsMessageTypeResolutionUpdateAck, *waitForAck ? &gfxMsg : nullptr);
  if (status != kIOReturnSuccess) {
    HVSYSLOG("Failed to send screen resolution with status 0x%X", status);
    return status;
  }

  _screenWidth  = *width;
  _screenHeight = *height;
  HVDBGLOG("Screen resolution is now set to %ux%ux%u", _screenWidth, _screenHeight, getScreenDepth());
  
  //
  // Reinitialize dirty tracking for new resolution.
  //
  cleanupDirtyTracking();
  initDirtyTracking();
  
  return kIOReturnSuccess;
}

IOReturn HyperVGraphics::setCursorShape(HyperVGraphicsPlatformFunctionSetCursorShapeParams *params, bool refreshCursor) {
  return _cmdGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &HyperVGraphics::setCursorShapeGated), params, &refreshCursor);
}

IOReturn HyperVGraphics::setCursorShapeGated(HyperVGraphicsPlatformFunctionSetCursorShapeParams *params, bool *refreshCursor) {
  UInt32   cursorSize = 0;
  IOReturn status;

  //
  // Validate cursor data.
  //
  if (params != nullptr) {
    //
    // Check that cursor is valid.
    //
    if ((params->width > kHyperVGraphicsCursorMaxWidth) || (params->height > kHyperVGraphicsCursorMaxHeight)
        || (params->hotX > params->width) || (params->hotY > params->height)) {
      HVSYSLOG("Invalid cursor image passed");
      return kIOReturnUnsupported;
    }
    cursorSize = (params->width * params->height * kHyperVGraphicsCursorARGBPixelSize);
    if (cursorSize > kHyperVGraphicsCursorMaxSize) {
      HVSYSLOG("Invalid cursor image passed");
      return kIOReturnUnsupported;
    }
    HVDBGLOG("Cursor data at %p size %ux%u hot %ux%u length %u bytes", params->cursorData,
             params->width, params->height, params->hotX, params->hotY, cursorSize);
  } else if (!(*refreshCursor)) {
    cursorSize = (1 * 1 * kHyperVGraphicsCursorARGBPixelSize);
    HVDBGLOG("No cursor data passed, setting to no cursor");
  }

  //
  // Allocate message if not already allocated.
  //
  if (_gfxMsgCursorShape == nullptr) {
    _gfxMsgCursorShapeSize = sizeof (*_gfxMsgCursorShape) + kHyperVGraphicsCursorMaxSize;
    _gfxMsgCursorShape = static_cast<HyperVGraphicsMessage*>(IOMalloc(_gfxMsgCursorShapeSize));
    if (_gfxMsgCursorShape == nullptr) {
      HVSYSLOG("Failed to allocate cursor graphics message");
      return kIOReturnNoResources;
    }
  }

  //
  // Check if we need to only resend the last sent data to Hyper-V.
  // This will occur when a feature change message is received.
  //
  if (!(*refreshCursor)) {
    //
    // Send cursor image.
    // Cursor format is ARGB if alpha is enabled, RGB otherwise.
    //
    _gfxMsgCursorShape->gfxHeader.type = kHyperVGraphicsMessageTypeCursorShape;
    _gfxMsgCursorShape->gfxHeader.size = sizeof (_gfxMsgCursorShape->gfxHeader) + sizeof (_gfxMsgCursorShape->cursorShape) + cursorSize;

    _gfxMsgCursorShape->cursorShape.partIndex = kHyperVGraphicsCursorPartIndexComplete;
    _gfxMsgCursorShape->cursorShape.isARGB    = 1;
    _gfxMsgCursorShape->cursorShape.width     = (params != nullptr) ? params->width  : 1;
    _gfxMsgCursorShape->cursorShape.height    = (params != nullptr) ? params->height : 1;
    _gfxMsgCursorShape->cursorShape.hotX      = (params != nullptr) ? params->hotX   : 0;
    _gfxMsgCursorShape->cursorShape.hotY      = (params != nullptr) ? params->hotY   : 0;

    if (params != nullptr) {
      //
      // Copy cursor data.
      // macOS provides cursor image inverted heightwise, flip here during the copy.
      //
      UInt32 stride = params->width * kHyperVGraphicsCursorARGBPixelSize;
      for (UInt32 dstY = 0, srcY = (params->height - 1); dstY < params->height; dstY++, srcY--) {
        memcpy(&_gfxMsgCursorShape->cursorShape.data[dstY * stride], &params->cursorData[srcY * stride], stride);
      }
    } else {
      //
      // For no cursor use 1x1 transparent square.
      //
      _gfxMsgCursorShape->cursorShape.data[0] = 0;
      _gfxMsgCursorShape->cursorShape.data[1] = 1;
      _gfxMsgCursorShape->cursorShape.data[2] = 1;
      _gfxMsgCursorShape->cursorShape.data[3] = 1;
    }
  } else {
    HVDBGLOG("Resending last cursor data");
  }

  //
  // Send cursor data to Hyper-V.
  //
  status = sendGraphicsMessage(_gfxMsgCursorShape);
  if (status != kIOReturnSuccess) {
    HVSYSLOG("Failed to send cursor shape with status 0x%X", status);
    return status;
  }
  HVDBGLOG("Set cursor data successfully");
  return kIOReturnSuccess;
}

IOReturn HyperVGraphics::setCursorPosition(SInt32 x, SInt32 y, bool isVisible, bool refreshCursor) {
  return _cmdGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &HyperVGraphics::setCursorPositionGated),
                             &x, &y, &isVisible, &refreshCursor);
}

IOReturn HyperVGraphics::setCursorPositionGated(SInt32 *x, SInt32 *y, bool *isVisible, bool *refreshCursor) {
  static SInt32 lastX         = 0;
  static SInt32 lastY         = 0;
  static bool   lastIsVisible = true;

  HyperVGraphicsMessage gfxMsg = { };
  IOReturn              status;

  //
  // Send cursor position and visibility.
  // Use previously saved data if a feature change message was received.
  //
  gfxMsg.gfxHeader.type = kHyperVGraphicsMessageTypeCursorPosition;
  gfxMsg.gfxHeader.size = sizeof (gfxMsg.gfxHeader) + sizeof (gfxMsg.cursorPosition);
  gfxMsg.cursorPosition.isVisible   = !(*refreshCursor) ? *isVisible : lastIsVisible;
  gfxMsg.cursorPosition.videoOutput = 0;
  gfxMsg.cursorPosition.x           = !(*refreshCursor) ? *x : lastX;
  gfxMsg.cursorPosition.y           = !(*refreshCursor) ? *y : lastY;

  status = sendGraphicsMessage(&gfxMsg);
  if (status != kIOReturnSuccess) {
    HVSYSLOG("Failed to send cursor position with status 0x%X", status);
  }

  if (!(*refreshCursor)) {
    lastX         = *x;
    lastY         = *y;
    lastIsVisible = *isVisible;
  }
  HVDBGLOG("Set cursor position to x %d y %d visible %u", lastX, lastY, lastIsVisible);
  return status;
}
