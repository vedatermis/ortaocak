// stb_image / stb_image_write / stb_dxt implementations for the assetc library only (the client has
// its own stb_image TU; never link both into one binary). Headers are external includes (/external:W0).

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG // embedded glTF images (generator output is often JPEG)
#define STBI_NO_STDIO
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>
