// Small wrappers around STB image loading for the vendored VkSplat trainer.
// The STB implementation is kept static inside stb_image_impl.cpp so it can coexist
// with the other STB translation units already present in osgVerse.
#pragma once

#include <cstdint>

namespace vksplat
{
    uint8_t* loadImage(const char* fileName, int* width, int* height, int* channels,
                       int desiredChannels);
    void freeImage(void* pixels);
}
