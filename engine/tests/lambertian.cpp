#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
using uint = unsigned;
using std::abs;
using std::sqrt;
struct float4 { float x,y,z,w; };
#include "../shaders/lambertian-sampling.hlsli"
#include "../shaders/fluid/liquid-root.hlsli"
using RGB = std::array<double,3>;
void require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }

// Integrate the entire uniform selection dimension at every discontinuity,
// independently of random-number luck. Exercise the actual shader policy.
void checkEstimator(const std::vector<RGB>& lights,RGB albedo,float budget,uint visibility,uint reference=0) {
    std::vector<float> weights,probabilities;float total=0;uint active=0;
    for(auto light:lights) {
        float w=float(std::max({light[0]*albedo[0],light[1]*albedo[1],light[2]*albedo[2]}));
        weights.push_back(w);total+=w;active+=w>0;
    }
    std::vector<double> cuts{0,1};double cumulative=0;
    for(float w:weights) {
        float p=lambertianProbability(w,total,active,budget,reference);
        require(w==0?p==0:(p>=.5f&&p<=1),"Nonzero light lost support or exceeded 2x compensation");
        if(reference||active<=budget)require(w==0||p==1,"Reference/small light set must be exact");
        probabilities.push_back(p);cumulative+=p;
        cuts.push_back(cumulative-std::floor(cumulative));
    }
    std::sort(cuts.begin(),cuts.end());
    RGB mean{},expected{};double meanRays=0;
    for(size_t k=1;k<cuts.size();++k) {
        double width=cuts[k]-cuts[k-1];if(width<1e-6)continue;
        float offset=float((cuts[k]+cuts[k-1])*.5);uint rays=0;
        for(size_t i=0;i<lights.size();++i) {
            float p=probabilities[i];bool selected=lambertianSelected(p,offset);
            offset=lambertianNextOffset(p,offset);
            require(offset>=0&&offset<1.000001f,"Selection offset escaped unit interval");
            if(selected) {
                require(p>0,"Selected a zero-probability light");++rays;
                if(visibility&(1u<<i))for(uint c=0;c<3;++c)mean[c]+=width*lights[i][c]*albedo[c]/p;
            }
        }
        require(rays>=uint(std::floor(cumulative+1e-5))&&rays<=uint(std::ceil(cumulative-1e-5)),"Unbounded systematic ray count");
        meanRays+=width*rays;
    }
    for(size_t i=0;i<lights.size();++i)if(visibility&(1u<<i))
        for(uint c=0;c<3;++c)expected[c]+=lights[i][c]*albedo[c];
    for(uint c=0;c<3;++c)require(abs(mean[c]-expected[c])<2e-5*std::max(1.,expected[c]),"Visibility reduction changed expected RGB energy");
    require(abs(meanRays-cumulative)<2e-5,"Inclusion probabilities do not match ray work");
}
int main() {
    try {
        const std::vector<RGB> lights{{1000,1,2},{.01,.2,4},{2,8,.1},{3,3,3},{0,0,0},{4,1,9},{1,2,1}};
        for(uint mask=0;mask<128;++mask)for(RGB albedo: {RGB{1,1,1},RGB{.01,.3,.9},RGB{0,0,0}})
            for(uint budget: {2u,4u,8u}) {
                checkEstimator(lights,albedo,budget,mask);
                checkEstimator(lights,albedo,budget,mask,1);
            }
        require(lambertianProbability(1000,1001,7,2,0)==1,"Dominant lighting should be deterministic");
        require(lambertianProbability(1,1001,7,2,0)==.5f,"Blocked dominant source starved a weak light");
        std::mt19937 rng(0x13225);std::uniform_real_distribution<float> u(0,1);
        for(uint samples=1;samples<=8;++samples)for(uint trial=0;trial<128;++trial) {
            std::vector<RGB> candidates(5+2*samples);
            for(auto& c:candidates)for(auto& channel:c)channel=double(u(rng))/samples;
            checkEstimator(candidates,{.82,.33,.06},2*samples,rng());
        }
        // Each two-light stratum keeps 1.5 visibility rays in expectation.
        // Exhaust all occlusion masks, saturated material channels and source
        // ratios, including a blocked dominant source and a lone visible light.
        for(uint mask=0;mask<4;++mask)for(uint trial=0;trial<1000;++trial) {
            std::vector<RGB> pair{{std::pow(10.,u(rng)*8-4),u(rng),u(rng)},{u(rng),u(rng),u(rng)}};
            checkEstimator(pair,{.03,.5,.9},1.5f,mask);
            checkEstimator(pair,{.03,.5,.9},1.5f,mask,1);
        }
        checkEstimator({RGB{0,0,0},RGB{1,2,3}},{1,1,1},1.5f,3);
        require(lambertianProbability(1,2,2,1.5f,0)*2==1.5f,"Paired light work reduction regressed");

        // Exhaustively compare root-existence decisions for arbitrary cubics,
        // plus thin sheets with equal-sign endpoints and multiple crossings.
        std::vector<float4> cubics{{1,0,0,0},{0,0,0,0},{-.5f,1,0,0},
            {.16f,-1,1,0},{-.08f,.66f,-1.5f,1},{.25f,-1,1,0},
            {.249999f,-1,1,0},{.01f,0,0,1}};
        for(uint i=0;i<100000;++i)cubics.push_back({u(rng)*2-1,u(rng)*2-1,u(rng)*2-1,u(rng)*2-1});
        for(auto cubic:cubics) {
            float exact=0,bracket=0;
            bool hit=liquidRoot(cubic,exact),fast=liquidRoot(cubic,bracket,true);
            require(hit==fast,"Boolean water traversal changed surface visibility");
            if(hit) {
                require(exact>=0&&exact<=1&&bracket>=0&&bracket<=1,"Water hit escaped clipped ray interval");
                require(abs(liquidPolynomial(cubic,exact))<1e-4,"Camera/photon root accuracy regressed");
            }
        }
        std::cout<<"Lambertian RGB expectation, bounded shadow work, and 100008 water-root comparisons passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
