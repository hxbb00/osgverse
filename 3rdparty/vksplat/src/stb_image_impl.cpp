// STB implementation for the vendored VkSplat trainer.
// STB_IMAGE_STATIC keeps the symbols local so this translation unit can coexist with
// the other STB implementations already present in osgVerse (plugins/readerwriter).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "stb_loader.h"

namespace vksplat
{

uint8_t* loadImage(const char* fileName, int* width, int* height, int* channels,
                   int desiredChannels)
{
    return stbi_load(fileName, width, height, channels, desiredChannels);
}

void freeImage(void* pixels)
{
    stbi_image_free(pixels);
}

}  // namespace vksplat
