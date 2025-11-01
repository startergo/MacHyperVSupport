//
//  HyperVGraphics.hpp
//  Hyper-V synthetic graphics driver
//
//  Copyright © 2025 Goldfish64. All rights reserved.
//

#ifndef HyperVGraphics_hpp
#define HyperVGraphics_hpp

#include <IOKit/IORangeAllocator.h>
#include <IOKit/IOService.h>

#include "HyperVVMBusDevice.hpp"
#include "HyperVGraphicsPlatformFunctions.hpp"
#include "HyperVGraphicsRegs.hpp"

class HyperVGraphics : public IOService {
  OSDeclareDefaultStructors(HyperVGraphics);
  HVDeclareLogFunctionsVMBusChild("gfx");
  typedef IOService super;

private:
  HyperVVMBusDevice   *_hvDevice          = nullptr;
  IOWorkLoop          *_workLoop          = nullptr;
  IOCommandGate       *_cmdGate           = nullptr;
  IOTimerEventSource  *_timerEventSource  = nullptr;
  

  VMBusVersion       _gfxVersion          = { };
  UInt32             _bitDepth            = 32; // TODO: Not all version support 32-bit
  IOPhysicalAddress  _gfxBase             = 0;
  UInt32             _gfxLength           = 0;
  bool               _gfxBaseAllocated    = false;
  UInt32        _screenWidth    = 0;
  UInt32        _screenHeight   = 0;
  bool          _fbReady        = false;

  //
  // Dirty rectangle tracking for optimized screen updates.
  //
  static constexpr UInt32 kDirtyTileSize = 64;  // 64x64 pixel tiles
  UInt32        _dirtyTilesX    = 0;  // Number of tiles horizontally
  UInt32        _dirtyTilesY    = 0;  // Number of tiles vertically
  UInt8         *_dirtyBitmap   = nullptr;  // Bitmap of dirty tiles
  UInt32        _dirtyBitmapSize = 0;
  bool          _fullScreenDirty = true;  // Force full update initially
  
  HyperVGraphicsMessage     *_gfxMsgCursorShape     = nullptr;
  size_t                    _gfxMsgCursorShapeSize  = 0;

  void handleRefreshTimer(IOTimerEventSource *sender);
  void handlePacket(VMBusPacketHeader *pktHeader, UInt32 pktHeaderLength, UInt8 *pktData, UInt32 pktDataLength);
  
  //
  // Internal functions.
  //
  inline UInt32 getScreenDepth() { return (_gfxVersion.value == kHyperVGraphicsVersionV3_0) ? kHyperVGraphicsBitDepth2008 : kHyperVGraphicsBitDepth; }
  IOReturn sendGraphicsMessage(HyperVGraphicsMessage *gfxMessage, HyperVGraphicsMessageType responseType = kHyperVGraphicsMessageTypeError,
                               HyperVGraphicsMessage *gfxMessageResponse = nullptr);
  IOReturn negotiateVersion(VMBusVersion version);
  IOReturn allocateGraphicsMemory(IOPhysicalAddress *outBase, UInt32 *outLength);
  
  //
  // Dirty rectangle tracking.
  //
  void initDirtyTracking();
  void cleanupDirtyTracking();
  void markFullScreenDirty();
  void markRegionDirty(UInt32 x, UInt32 y, UInt32 width, UInt32 height);
  bool isDirty();
  UInt32 buildDirtyRectangles(HyperVGraphicsImageUpdateRectangle *rects, UInt32 maxRects);
  void clearDirtyFlags();
  
  IOReturn refreshFramebufferImage();
  IOReturn setGraphicsMemory(IOPhysicalAddress base, UInt32 length);
  IOReturn setScreenResolution(UInt32 width, UInt32 height, bool waitForAck = true);
  IOReturn setScreenResolutionGated(UInt32 *width, UInt32 *height, bool *waitForAck);
  IOReturn setCursorShape(HyperVGraphicsPlatformFunctionSetCursorShapeParams *params, bool refreshCursor = false);
  IOReturn setCursorShapeGated(HyperVGraphicsPlatformFunctionSetCursorShapeParams *params, bool *refreshCursor);
  IOReturn setCursorPosition(SInt32 x, SInt32 y, bool isVisible, bool refreshCursor = false);
  IOReturn setCursorPositionGated(SInt32 *x, SInt32 *y, bool *isVisible, bool *refreshCursor);

  //
  // Platform functions.
  //
  IOReturn platformInitGraphics(VMBusVersion *outVersion, IOPhysicalAddress *outMemBase, UInt32 *outMemLength);
  IOReturn platformSetScreenResolution(UInt32 *inWidth, UInt32 *inHeight);
  IOReturn platformSetCursorPosition(SInt32 *x, SInt32 *y, bool *isVisible);

public:
  //
  // IOService overrides.
  //
  bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
  void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
  IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction,
                                void *param1, void *param2, void *param3, void *param4) APPLE_KEXT_OVERRIDE;
};

#endif
