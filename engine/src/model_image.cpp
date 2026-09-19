#include "model_asset.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 16384
#include <stb_image.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace lab::asset {
void buildMips(Image &image) {
    auto decode = [&](uint8_t b, unsigned c) {
        float v = b / 255.f;
        return image.srgb && c < 3 ? (v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f)) : v;
    };
    auto encode = [&](float v, unsigned c) {
        if (image.srgb && c < 3)
            v = v <= .0031308f ? 12.92f * v : 1.055f * std::pow(v, 1 / 2.4f) - .055f;
        return uint8_t(std::clamp(std::lround(v * 255), 0l, 255l));
    };
    while (image.mips.back().width > 1 || image.mips.back().height > 1) {
        const auto &src = image.mips.back();
        Mip dst{std::max(1u, src.width / 2), std::max(1u, src.height / 2), {}};
        dst.rgba.resize(size_t(dst.width) * dst.height * 4);
        for (uint32_t y = 0; y < dst.height; ++y)
            for (uint32_t x = 0; x < dst.width; ++x)
                for (unsigned c = 0; c < 4; ++c) {
                    float sum = 0;
                    const auto x0 = x * src.width / dst.width, x1 = (x + 1) * src.width / dst.width;
                    const auto y0 = y * src.height / dst.height, y1 = (y + 1) * src.height / dst.height;
                    for (auto sy = y0; sy < y1; ++sy)
                        for (auto sx = x0; sx < x1; ++sx)
                            sum += decode(src.rgba[(size_t(sy) * src.width + sx) * 4 + c], c);
                    dst.rgba[(size_t(y) * dst.width + x) * 4 + c] = encode(sum / ((x1 - x0) * (y1 - y0)), c);
                }
        image.mips.push_back(std::move(dst));
    }
}
Image decodeImage(const std::string &name, const uint8_t *bytes, size_t size, bool srgb) {
    if (!size || size > INT32_MAX)
        throw std::runtime_error("Invalid image size: " + name);
    int w = 0, h = 0, components = 0;
    if (!stbi_info_from_memory(bytes, int(size), &w, &h, &components) || w <= 0 || h <= 0 ||
        w > 16384 || h > 16384 || uint64_t(w) * h > 67108864)
        throw std::runtime_error("Invalid or oversized image: " + name);
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded(
        stbi_load_from_memory(bytes, int(size), &w, &h, &components, 4), stbi_image_free);
    if (!decoded)
        throw std::runtime_error("Cannot decode image " + name + ": " + stbi_failure_reason());
    Image image{name, srgb, {}};
    image.mips.push_back({uint32_t(w), uint32_t(h), {decoded.get(), decoded.get() + size_t(w) * h * 4}});
    buildMips(image);
    return image;
}
} // namespace lab::asset
