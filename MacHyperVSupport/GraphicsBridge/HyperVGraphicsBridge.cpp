//
//  HyperVGraphicsBridge.cpp
//  Hyper-V synthetic graphics bridge
//
//  Copyright © 2021-2025 Goldfish64. All rights reserved.
//

#include "HyperVGraphicsBridge.hpp"
#include "HyperVGraphics.hpp"
#include "HyperVPCIRoot.hpp"

OSDefineMetaClassAndStructors(HyperVGraphicsBridge, super);

bool HyperVGraphicsBridge::init(OSDictionary *dictionary) {
  if (!super::init(dictionary)) {
    return false;
  }

  //
  // Allocate PCI lock early to prevent spinlock timeout if config space
  // is accessed before start() completes. The lock must exist throughout
  // the object's lifetime.
  //
  _pciLock = IOSimpleLockAlloc();
  if (_pciLock == nullptr) {
    return false;
  }

  //
  // Zero out fake PCI device space.
  // We cannot populate valid PCI config values here because we need
  // console info (PE_Video) from the provider, which isn't available
  // until start(). The config space will be populated in start() before
  // we register with the PCI root and call super::start().
  //
  bzero(_fakePCIDeviceSpace, sizeof(_fakePCIDeviceSpace));

  return true;
}

void HyperVGraphicsBridge::free() {
  //
  // Free PCI lock if allocated.
  //
  if (_pciLock != nullptr) {
    IOSimpleLockFree(_pciLock);
    _pciLock = nullptr;
  }

  super::free();
}

IOService* HyperVGraphicsBridge::probe(IOService *provider, SInt32 *score) {
  HyperVPCIRoot *hvPCIRoot;

  HVCheckDebugArgs();

  //
  // Ensure parent is HyperVGraphics object and locate root PCI bus instance.
  //
  if (OSDynamicCast(HyperVGraphics, provider) == nullptr) {
    HVSYSLOG("Provider is not HyperVGraphics");
    return nullptr;
  }
  hvPCIRoot = HyperVPCIRoot::getPCIRootInstance();
  if (hvPCIRoot == nullptr) {
    HVSYSLOG("Failed to find root PCI bridge instance");
    return nullptr;
  }

  //
  // Do not start on Gen1 VMs.
  //
  if (!hvPCIRoot->isHyperVGen2()) {
    HVDBGLOG("Not starting on Hyper-V Gen1 VM");
    return nullptr;
  }
  return super::probe(provider, score);
}

bool HyperVGraphicsBridge::start(IOService *provider) {
  PE_Video consoleInfo = { };
  HyperVPCIRoot *hvPCIRoot;
  IOReturn status;

  HVCheckDebugArgs();
  HVDBGLOG("Initializing Hyper-V Synthetic Graphics Bridge");

  if (HVCheckOffArg()) {
    HVSYSLOG("Disabling Hyper-V Synthetic Graphics Bridge due to boot arg");
    return false;
  }

  //
  // Pull console info.
  //
  if (getPlatform()->getConsoleInfo(&consoleInfo) != kIOReturnSuccess) {
    HVSYSLOG("Failed to get console info");
    return false;
  }
  HVDBGLOG("Console is at 0x%X (%ux%u, bpp: %u, bytes/row: %u)",
         consoleInfo.v_baseAddr, consoleInfo.v_width, consoleInfo.v_height, consoleInfo.v_depth, consoleInfo.v_rowBytes);
  _fbInitialBase   = (UInt32)consoleInfo.v_baseAddr;
  _fbInitialLength = (UInt32)(consoleInfo.v_height * consoleInfo.v_rowBytes);

  //
  // Locate root PCI bus instance.
  //
  hvPCIRoot = HyperVPCIRoot::getPCIRootInstance();
  if (hvPCIRoot == nullptr) {
    HVSYSLOG("Failed to find root PCI bridge instance");
    return false;
  }

  //
  // Fill PCI device config space with actual values BEFORE registering.
  // PCI bridge will contain a single PCI graphics device with the
  // framebuffer memory at BAR0. The vendor/device ID is the same as
  // what a generation 1 Hyper-V VM uses for the emulated graphics.
  //
  // IMPORTANT: This must be done before registerChildPCIBridge() to avoid
  // IOGraphicsFamily seeing invalid/zeroed PCI config space and failing
  // to attach. The root PCI bridge only forwards config-space access to
  // child bridges after registration, so this prevents early probing.
  //
  OSWriteLittleInt16(_fakePCIDeviceSpace, kIOPCIConfigVendorID, kHyperVPCIVendorMicrosoft);
  OSWriteLittleInt16(_fakePCIDeviceSpace, kIOPCIConfigDeviceID, kHyperVPCIDeviceHyperVVideo);
  OSWriteLittleInt32(_fakePCIDeviceSpace, kIOPCIConfigRevisionID, 0x3000000);
  OSWriteLittleInt16(_fakePCIDeviceSpace, kIOPCIConfigSubSystemVendorID, kHyperVPCIVendorMicrosoft);
  OSWriteLittleInt16(_fakePCIDeviceSpace, kIOPCIConfigSubSystemID, kHyperVPCIDeviceHyperVVideo);
  OSWriteLittleInt32(_fakePCIDeviceSpace, kIOPCIConfigBaseAddress0, (UInt32)_fbInitialBase);

  //
  // Register with root PCI bridge AFTER populating config space.
  // This prevents HyperVPCIRoot from forwarding config reads/writes to us
  // until we have valid data ready.
  //
  status = hvPCIRoot->registerChildPCIBridge(this, &_pciBusNumber);
  if (status != kIOReturnSuccess) {
    HVSYSLOG("Failed to register with root PCI bus instance");
    return false;
  }

  //
  // Call super::start() AFTER registration so IOKit can properly discover
  // and attach child devices with valid PCI config space.
  //
  if (!super::start(provider)) {
    HVSYSLOG("super::start() returned false");
    return false;
  }

  HVDBGLOG("Initialized Hyper-V Synthetic Graphics Bridge");
  return true;
}

void HyperVGraphicsBridge::stop(IOService *provider) {
  HVDBGLOG("Hyper-V Synthetic Graphics Bridge is stopping");

  //
  // Note: _pciLock is freed in free(), not here, to ensure it's
  // available during the entire object lifetime.
  //

  super::stop(provider);
}

bool HyperVGraphicsBridge::configure(IOService *provider) {
  //
  // Add framebuffer memory range to bridge.
  //
  HVDBGLOG("Adding framebuffer memory 0x%X length 0x%X to PCI bridge", _fbInitialBase, _fbInitialLength);
  addBridgeMemoryRange(_fbInitialBase, _fbInitialLength, true);
  return super::configure(provider);
}

UInt32 HyperVGraphicsBridge::configRead32(IOPCIAddressSpace space, UInt8 offset) {
  UInt32 data;
  IOInterruptState ints;

  if (space.es.deviceNum != 0 || space.es.functionNum != 0) {
    return 0xFFFFFFFF;
  }

  if (_pciLock == nullptr) {
    HVDBGLOG("Warning: PCI lock not initialized, returning safe value");
    return OSReadLittleInt32(_fakePCIDeviceSpace, offset);
  }

  ints = IOSimpleLockLockDisableInterrupt(_pciLock);
  data = OSReadLittleInt32(_fakePCIDeviceSpace, offset);
  IOSimpleLockUnlockEnableInterrupt(_pciLock, ints);

  HVDBGLOG("Read 32-bit value %u from offset 0x%X", data, offset);
  return data;
}

void HyperVGraphicsBridge::configWrite32(IOPCIAddressSpace space, UInt8 offset, UInt32 data) {
  IOInterruptState ints;
  
  if (space.es.deviceNum != 0 || space.es.functionNum != 0
      || (offset > kIOPCIConfigurationOffsetBaseAddress0 && offset <= kIOPCIConfigurationOffsetBaseAddress5)
      || offset == kIOPCIConfigurationOffsetExpansionROMBase) {
    return;
  }
  HVDBGLOG("Writing 32-bit value %u to offset 0x%X", data, offset);

  //
  // Return BAR0 size if requested.
  //
  if (offset == kIOPCIConfigurationOffsetBaseAddress0 && data == 0xFFFFFFFF) {
    OSWriteLittleInt32(_fakePCIDeviceSpace, offset, (0xFFFFFFFF - _fbInitialLength) + 1);
    return;
  }

  if (_pciLock == nullptr) {
    HVDBGLOG("Warning: PCI lock not initialized");
    OSWriteLittleInt32(_fakePCIDeviceSpace, offset, data);
    return;
  }

  ints = IOSimpleLockLockDisableInterrupt(_pciLock);
  OSWriteLittleInt32(_fakePCIDeviceSpace, offset, data);
  IOSimpleLockUnlockEnableInterrupt(_pciLock, ints);
}

UInt16 HyperVGraphicsBridge::configRead16(IOPCIAddressSpace space, UInt8 offset) {
  UInt16 data;
  IOInterruptState ints;

  if (space.es.deviceNum != 0 || space.es.functionNum != 0) {
    return 0xFFFF;
  }

  //
  // Safety check: _pciLock should always be allocated in init(),
  // but check to prevent null pointer dereference.
  //
  if (_pciLock == nullptr) {
    HVDBGLOG("Warning: PCI lock not initialized, returning safe value");
    return OSReadLittleInt16(_fakePCIDeviceSpace, offset);
  }

  ints = IOSimpleLockLockDisableInterrupt(_pciLock);
  data = OSReadLittleInt16(_fakePCIDeviceSpace, offset);
  IOSimpleLockUnlockEnableInterrupt(_pciLock, ints);

  HVDBGLOG("Read 16-bit value %u from offset 0x%X", data, offset);
  return data;
}

void HyperVGraphicsBridge::configWrite16(IOPCIAddressSpace space, UInt8 offset, UInt16 data) {
  IOInterruptState ints;

  if (space.es.deviceNum != 0 || space.es.functionNum != 0
      || (offset >= kIOPCIConfigurationOffsetBaseAddress0 && offset <= kIOPCIConfigurationOffsetBaseAddress5)
      || offset == kIOPCIConfigurationOffsetExpansionROMBase) {
    return;
  }
  HVDBGLOG("Writing 16-bit value %u to offset 0x%X", data, offset);

  if (_pciLock == nullptr) {
    HVDBGLOG("Warning: PCI lock not initialized");
    OSWriteLittleInt16(_fakePCIDeviceSpace, offset, data);
    return;
  }

  ints = IOSimpleLockLockDisableInterrupt(_pciLock);
  OSWriteLittleInt16(_fakePCIDeviceSpace, offset, data);
  IOSimpleLockUnlockEnableInterrupt(_pciLock, ints);
}

UInt8 HyperVGraphicsBridge::configRead8(IOPCIAddressSpace space, UInt8 offset) {
  UInt8 data;
  IOInterruptState ints;

  if (space.es.deviceNum != 0 || space.es.functionNum != 0) {
    return 0xFF;
  }

  if (_pciLock == nullptr) {
    HVDBGLOG("Warning: PCI lock not initialized, returning safe value");
    return _fakePCIDeviceSpace[offset];
  }

  ints = IOSimpleLockLockDisableInterrupt(_pciLock);
  data = _fakePCIDeviceSpace[offset];
  IOSimpleLockUnlockEnableInterrupt(_pciLock, ints);

  HVDBGLOG("Read 8-bit value %u from offset 0x%X", data, offset);
  return data;
}

void HyperVGraphicsBridge::configWrite8(IOPCIAddressSpace space, UInt8 offset, UInt8 data) {
  IOInterruptState ints;

  if (space.es.deviceNum != 0 || space.es.functionNum != 0
      || (offset >= kIOPCIConfigurationOffsetBaseAddress0 && offset <= kIOPCIConfigurationOffsetBaseAddress5)
      || offset == kIOPCIConfigurationOffsetExpansionROMBase) {
    return;
  }
  HVDBGLOG("Writing 8-bit value %u to offset 0x%X", data, offset);

  if (_pciLock == nullptr) {
    HVDBGLOG("Warning: PCI lock not initialized");
    _fakePCIDeviceSpace[offset] = data;
    return;
  }

  ints = IOSimpleLockLockDisableInterrupt(_pciLock);
  _fakePCIDeviceSpace[offset] = data;
  IOSimpleLockUnlockEnableInterrupt(_pciLock, ints);
}
