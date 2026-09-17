#include "../src/water.h"
#include "../src/scene.h"
#include <iostream>
#include <map>
#include <tuple>
#include <stdexcept>
void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
int main() {
    try {
        // IAPWS R9-97 verification table: 0 C, 0.1 MPa, density 999.843 kg/m^3.
        require(std::abs(lab::waterIndex(589, 273.15, 999.843) - 1.334344) < .000001,
                "IAPWS 589 nm reference failed");
        require(std::abs(lab::waterIndex(226.5, 273.15, 999.843) - 1.394527) < .000001,
                "IAPWS UV reference failed");
        require(std::abs(lab::waterIndex(1013.98, 273.15, 999.843) - 1.326135) < .000001,
                "IAPWS IR reference failed");
        require(std::abs(lab::waterAbsorption(550) - .0565f) < 1e-6,
                "Water absorption cm-to-m conversion failed");
        require(std::abs(lab::waterAbsorption(532) - .04444f) < 1e-6, "Water spectral interpolation failed");
        require(lab::waterAbsorption(638) > lab::waterAbsorption(450) * 30,
                "Red/blue water absorption missing");
        for (int nm = 381; nm <= 780; ++nm) {
            require(lab::waterIndex(nm) < lab::waterIndex(nm - 1), "Nonphysical water dispersion");
            require(std::isfinite(lab::waterAbsorption(float(nm))) && lab::waterAbsorption(float(nm)) > 0,
                    "Invalid water absorption");
        }
        for (bool flat : {false, true}) {
            auto scene = lab::makePlayScene(true, flat);
            require(scene.size() == 7 && scene.back().mask == 8, "Water is not a separate DXR optical mesh");
            using Point = std::tuple<long, long, long>;
            using Edge = std::pair<Point, Point>;
            std::map<Edge, std::pair<int, int>> edges;
            auto key = [](auto p) {
                return Point{std::lround(p.x * 10000), std::lround(p.y * 10000), std::lround(p.z * 10000)};
            };
            const auto &v = scene.back().vertices;
            double signedVolume = 0;
            for (size_t i = 0; i < v.size(); i += 3) {
                auto a = v[i].position, b = v[i + 1].position, c = v[i + 2].position;
                signedVolume += (a.x * (b.y * c.z - b.z * c.y) + a.y * (b.z * c.x - b.x * c.z) +
                                 a.z * (b.x * c.y - b.y * c.x)) /
                                6;
                for (int j = 0; j < 3; ++j) {
                    auto a = key(v[i + j].position), b = key(v[i + (j + 1) % 3].position);
                    require(a != b, "Degenerate water triangle");
                    bool forward = a < b;
                    auto &e = edges[forward ? Edge{a, b} : Edge{b, a}];
                    ++e.first;
                    e.second += forward ? 1 : -1;
                }
            }
            for (auto &[edge, count] : edges)
                require(count.first == 2 && count.second == 0,
                        "Water boundary is not watertight/outward oriented");
            require(signedVolume > 15 && signedVolume < 18, "Water volume/orientation invalid");
            std::cout << "water flat=" << flat << " triangles=" << v.size() / 3 << " volume=" << signedVolume
                      << " m^3\n";
        }
        std::cout << "PASS: IAPWS reference indices, measured absorption/units, spectral interpolation and "
                     "watertight tank geometry\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
