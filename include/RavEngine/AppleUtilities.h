#pragma once
#include <cstdint>
#include <cstddef>
#include <RGL/Types.hpp>

/**
 Resize a metal layer manually. Required on iOS
 @param ptr the pointer to the CAMetalLayer
 @param width the new width, in points
 @param height the new height, in points
 */
void resizeMetalLayer(void* ptr, int width, int height);

/**
 Get the scale factor on macOS or iOS
 @param window the SDL window pointer
 @return the scale factor returned by the Apple API
 */
float GetWindowScaleFactor(void* window);

/**
 SDL opts-out of inertial scrolling on macOS. This function re-enables it.
 */
void enableSmoothScrolling();

struct AppleOSVersion{
    uint16_t major = 0, minor = 0, patch = 0;
};
AppleOSVersion GetAppleOSVersion();

void AppleOSName(char* buffer, uint16_t size);

/**
 @return total system memory in MB
 */
uint32_t GetAppleSystemRAM();

void AppleCPUName(char* buffer, size_t size);

bool AppleGPUMeetsMinSpec(RGLDevicePtr);

uint32_t AppleVRAMUsed();
uint32_t AppleVRAMTotal();

void AppleAutoreleasePoolInit();
void AppleAutoreleasePoolDrain();
