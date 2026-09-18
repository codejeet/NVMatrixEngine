#pragma once
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace lab::ocean {
// Radiance RGBE remains linear HDR; never pass through an 8-bit/sRGB decoder.
inline std::vector<DirectX::XMFLOAT4> loadEnvironment(const std::filesystem::path &folder) {
    using DirectX::XMFLOAT4;
    std::vector<XMFLOAT4> result(4);
    unsigned expectedWidth = 0, expectedHeight = 0;
    for (unsigned layer = 0; layer < 2; ++layer) {
        std::ifstream in(folder / (layer ? "night.hdr" : "day.hdr"), std::ios::binary);
        if (!in) throw std::runtime_error("Ocean HDRI missing; rebuild/copy assets/ocean");
        std::string line;
        bool format = false;
        while (std::getline(in, line) && !line.empty() && line != "\r")
            format |= line.find("FORMAT=32-bit_rle_rgbe") != std::string::npos;
        std::getline(in, line);
        std::istringstream dimensions(line);
        std::string ySign, xSign;
        unsigned width = 0, height = 0;
        dimensions >> ySign >> height >> xSign >> width;
        if (!format || ySign != "-Y" || xSign != "+X" || width < 8 || width > 4096 || !height || height > 2048)
            throw std::runtime_error("Unsupported ocean Radiance HDRI");
        if (layer && (width != expectedWidth || height != expectedHeight))
            throw std::runtime_error("Ocean HDRI sizes differ");
        expectedWidth = width; expectedHeight = height;
        const size_t base = result.size(), count = size_t(width) * height;
        // Conditional column CDFs plus a row CDF avoid losing dim sky pixels
        // to float precision after a bright sun in one 524,288-entry CDF.
        result.resize(base + count + height);
        std::vector<unsigned char> scanline(width * 4);
        auto byte = [&]() {
            int b = in.get();
            if (b < 0) throw std::runtime_error("Truncated ocean HDRI");
            return unsigned(b);
        };
        for (unsigned y = 0; y < height; ++y) {
            const auto r = byte(), g = byte(), hi = byte(), lo = byte();
            if (r != 2 || g != 2 || (hi * 256 + lo) != width) throw std::runtime_error("Invalid HDRI scanline");
            for (unsigned channel = 0; channel < 4; ++channel)
                for (unsigned x = 0; x < width;) {
                    unsigned code = byte(), length = code > 128 ? code - 128 : code;
                    if (!length || x + length > width) throw std::runtime_error("Invalid HDRI RLE run");
                    if (code > 128) {
                        unsigned value = byte();
                        for (unsigned i = 0; i < length; ++i) scanline[channel * width + x++] = uint8_t(value);
                    } else
                        for (unsigned i = 0; i < length; ++i) scanline[channel * width + x++] = uint8_t(byte());
                }
            for (unsigned x = 0; x < width; ++x) {
                unsigned e = scanline[3 * width + x];
                float scale = e ? std::ldexp(1.f, int(e) - 136) : 0;
                result[base + y * width + x] = {(scanline[x] + .5f) * scale,
                    (scanline[width + x] + .5f) * scale, (scanline[2 * width + x] + .5f) * scale, 0};
            }
        }
        constexpr double pi = 3.141592653589793;
        auto luminance = [](XMFLOAT4 p) { return .2126 * p.x + .7152 * p.y + .0722 * p.z; };
        double horizontal = 0, peak = 0;
        size_t brightest = 0;
        for (unsigned y = 0; y < height; ++y) {
            double omega = 2 * pi / width * (std::cos(pi*y/height) - std::cos(pi*(y+1)/height));
            double cosine = std::max(0., std::cos(pi*(y+.5)/height));
            for (unsigned x = 0; x < width; ++x) {
                size_t i = size_t(y) * width + x;
                double value = luminance(result[base+i]);
                horizontal += value * omega * cosine;
                if (cosine > 0 && value > peak) { peak = value; brightest = i; }
            }
        }
        if (!(horizontal > 0) || !std::isfinite(horizontal)) throw std::runtime_error("Invalid HDRI radiance");
        // Reference horizontal illuminance, then a photographic exposure change.
        // These are calibrated presets, not a claim that the captures are lux meters.
        const double lux = layer ? .002 : 80000, exposure = layer ? 4096 : 1./128;
        const double scale = lux * exposure / (horizontal * 182.458);
        std::vector<double> rows(height), cumulative(width);
        double sum = 0, sun = 0;
        for (unsigned y = 0; y < height; ++y) {
            double omega = 2 * pi / width * (std::cos(pi*y/height) - std::cos(pi*(y+1)/height));
            double row = 0;
            for (unsigned x = 0; x < width; ++x) {
                size_t i = size_t(y) * width + x;
                auto &p = result[base+i];
                double value = luminance(p);
                if (value > peak*.05) sun += value*scale*omega*std::max(0., std::cos(pi*(y+.5)/height));
                row += std::max(value, peak*1e-7);
                cumulative[x] = row;
                p.x *= float(scale); p.y *= float(scale); p.z *= float(scale);
            }
            sum += row * omega;
            rows[y] = sum;
            // Uniform mixture retains support for bilinear filtering and makes
            // every probability resolvable in the GPU's float CDFs.
            for (unsigned x = 0; x < width; ++x)
                result[base + size_t(y)*width + x].w = float(.999*cumulative[x]/row + .001*(x+1)/width);
            result[base + size_t(y+1)*width - 1].w = 1;
        }
        for (unsigned y = 0; y < height; ++y)
            result[base+count+y].x = float(.999*rows[y]/sum + .001*(y+1)/height);
        result.back().x = 1;
        double theta = pi * (brightest / width + .5) / height;
        double phi = 2*pi * (brightest % width + .5) / width;
        result[1+layer] = {float(std::sin(theta)*std::cos(phi)), float(std::cos(theta)),
                           float(std::sin(theta)*std::sin(phi)), layer ? 0.f : float(sun)};
    }
    result[0] = {float(expectedWidth), float(expectedHeight), 80000, .002f};
    result[3] = {1.f/128, 4096, 0, 0};
    return result;
}
}
