// The one stb_image implementation of the app. Upstream defines it in the SDL window's source file (graphics/presentation/window/window.cpp), which the
// UWP app replaces.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#include "stb_image.h"
