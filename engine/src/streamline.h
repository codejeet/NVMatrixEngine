#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <sl.h>
#include <sl_dlss_d.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <ostream>
#include "camera.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
using namespace DirectX;
void logLine(const std::string &text);
void slCheck(sl::Result result, const char *operation);
namespace lab {
class Streamline {
  public:
    ~Streamline();
    void init(const std::filesystem::path &folder);
    void setDevice(ID3D12Device *device, const LUID &luid);
    void upgrade(void **object);
    void shutdown();
    void free();
    std::pair<uint32_t, uint32_t> configure(uint32_t width, uint32_t height, int quality);
    void evaluate(ID3D12GraphicsCommandList *commands, const std::array<ID3D12Resource *, 8> &textures,
                  const Camera &camera, const Camera &previous, XMFLOAT2 jitter, bool reset, uint32_t frame);
    uint64_t evaluations = 0;
    bool active = false;
    uint32_t renderWidth = 0, renderHeight = 0, width = 0, height = 0;
    void loadFrameGeneration(bool enabled); // caller releases swapchain first
    void setMultiplier(uint32_t value);
    void beginFrame(uint32_t index);
    void marker(sl::PCLMarker value);
    void endPresent(ID3D12CommandQueue *queue);
    void suspend();
    void prepareFrame(bool gameFrame);
    void tagFrameGeneration(ID3D12GraphicsCommandList *, ID3D12Resource *depth, ID3D12Resource *motion,
                            ID3D12Resource *hudless, ID3D12Resource *ui, ID3D12Resource *distortion);
    void inputMessage(UINT message, bool click);
    void report(std::ostream &) const;
    bool fgSupported = false, fgLoaded = false, fgEnabled = false, reflexAvailable = false;
    bool vsyncSupported = false;
    uint32_t multiplier = 1, maxMultiplier = 1, minimumDimension = 0;
    uint32_t lastPresented = 1, fgStatus = 0;
    uint64_t presentedFrames = 0, interpolatedPresents = 0;
    bool distortionTagged = false;
    uint64_t distortionFrames = 0;
    std::string fgReason;

  private:
    HMODULE module = nullptr;
    sl::ViewportHandle viewport{0};
    sl::DLSSDOptions options{};
    decltype(&slInit) initFn = nullptr;
    decltype(&slShutdown) shutdownFn = nullptr;
    decltype(&slSetD3DDevice) deviceFn = nullptr;
    decltype(&slUpgradeInterface) upgradeFn = nullptr;
    decltype(&slIsFeatureSupported) supportedFn = nullptr;
    decltype(&slGetFeatureFunction) featureFn = nullptr;
    decltype(&slGetNewFrameToken) tokenFn = nullptr;
    decltype(&slSetConstants) constantsFn = nullptr;
    decltype(&slSetTagForFrame) tagFn = nullptr;
    decltype(&slEvaluateFeature) evaluateFn = nullptr;
    decltype(&slFreeResources) freeFn = nullptr;
    decltype(&slSetFeatureLoaded) loadedFn = nullptr;
    PFun_slDLSSDGetOptimalSettings *optimalFn = nullptr;
    PFun_slDLSSDSetOptions *optionsFn = nullptr;
    PFun_slDLSSGGetState *fgStateFn = nullptr;
    PFun_slDLSSGSetOptions *fgOptionsFn = nullptr;
    PFun_slReflexSleep *sleepFn = nullptr;
    PFun_slReflexSetOptions *reflexOptionsFn = nullptr;
    PFun_slPCLSetMarker *markerFn = nullptr;
    sl::DLSSGOptions fgOptions{};
    sl::FrameToken *currentToken = nullptr;
    bool pingPending = false, clickPending = false, frameInProgress = false;
    uint32_t pingMessage = 0;
    uint64_t simulationMarkers = 0, submissionMarkers = 0, presentMarkers = 0, frameTokens = 0;
    uint64_t resumeAfterPresent = 0;
    void importFrameGeneration();
    void clearFrameGenerationTags();
};

} // namespace lab
