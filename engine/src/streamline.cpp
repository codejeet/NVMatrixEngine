#include "streamline.h"
#include <sl_security.h>
#include <fstream>
#include <mutex>
#include <stdexcept>
namespace {
std::mutex logMutex;
void callback(sl::LogType type, const char *message) {
    logLine(std::string(type == sl::LogType::eError  ? "SL ERROR: "
                        : type == sl::LogType::eWarn ? "SL WARN: "
                                                     : "SL: ") +
            message);
}
sl::float4x4 matrix(FXMMATRIX m) {
    XMFLOAT4X4 f;
    XMStoreFloat4x4(&f, m);
    sl::float4x4 result;
    memcpy(&result, &f, sizeof(f));
    return result;
}
template <class T> void function(HMODULE module, const char *name, T &target) {
    target = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!target)
        throw std::runtime_error(std::string("Missing Streamline export: ") + name);
}
} // namespace
void logLine(const std::string &text) {
    std::lock_guard<std::mutex> lock(logMutex);
    std::ofstream("NVMatrixEngine.log", std::ios::app) << text << '\n';
    OutputDebugStringA((text + "\n").c_str());
}
void slCheck(sl::Result result, const char *operation) {
    if (result != sl::Result::eOk)
        throw std::runtime_error(std::string(operation) + " failed (SL " + std::to_string(int(result)) +
                                 "). See NVMatrixEngine.log.");
}
namespace lab {
void Streamline::init(const std::filesystem::path &folder) {
    const auto path = std::filesystem::absolute(folder / "sl.interposer.dll");
    if (!sl::security::verifyEmbeddedSignature(path.c_str()))
        throw std::runtime_error(
            "NVIDIA Streamline DLL signature verification failed. Restore the official SDK binaries.");
    module = LoadLibraryExW(path.c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module)
        throw std::runtime_error("Cannot load signed sl.interposer.dll");
#define IMPORT(member, name) function(module, #name, member)
    IMPORT(initFn, slInit);
    IMPORT(shutdownFn, slShutdown);
    IMPORT(deviceFn, slSetD3DDevice);
    IMPORT(upgradeFn, slUpgradeInterface);
    IMPORT(supportedFn, slIsFeatureSupported);
    IMPORT(featureFn, slGetFeatureFunction);
    IMPORT(tokenFn, slGetNewFrameToken);
    IMPORT(constantsFn, slSetConstants);
    IMPORT(tagFn, slSetTagForFrame);
    IMPORT(evaluateFn, slEvaluateFeature);
    IMPORT(freeFn, slFreeResources);
    IMPORT(loadedFn, slSetFeatureLoaded);
#undef IMPORT
    const auto pluginFolder = folder.wstring();
    const wchar_t *paths[] = {pluginFolder.c_str()};
    sl::Feature features[] = {sl::kFeatureDLSS_RR, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences prefs{};
    prefs.featuresToLoad = features;
    prefs.numFeaturesToLoad = uint32_t(std::size(features));
    prefs.pathsToPlugins = paths;
    prefs.numPathsToPlugins = 1;
    prefs.pathToLogsAndData = pluginFolder.c_str();
    prefs.logMessageCallback = callback;
    prefs.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eDisableCLStateTracking |
                  sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    // Use our own project identity, never a borrowed NVIDIA application ID.
    prefs.engine = sl::EngineType::eCustom;
    prefs.engineVersion = "NVMatrixEngine-0.2";
    prefs.projectId = "6da166aa-a682-46e6-9834-5a5d72ebf83d";
    prefs.renderAPI = sl::RenderAPI::eD3D12;
    slCheck(initFn(prefs, sl::kSDKVersion), "slInit");
    active = true;
    logLine("Streamline 2.12.0 initialized; pinned local binaries, OTA disabled.");
}
void Streamline::upgrade(void **object) {
    slCheck(upgradeFn(object), "slUpgradeInterface");
}
void Streamline::setDevice(ID3D12Device *device, const LUID &luid) {
    sl::AdapterInfo adapter{};
    adapter.deviceLUID = reinterpret_cast<uint8_t *>(const_cast<LUID *>(&luid));
    adapter.deviceLUIDSizeInBytes = sizeof(luid);
    slCheck(supportedFn(sl::kFeatureDLSS_RR, adapter), "DLSS Ray Reconstruction support check");
    slCheck(deviceFn(device), "slSetD3DDevice");
    slCheck(featureFn(sl::kFeatureDLSS_RR, "slDLSSDGetOptimalSettings", reinterpret_cast<void *&>(optimalFn)),
            "Import DLSSDGetOptimalSettings");
    slCheck(featureFn(sl::kFeatureDLSS_RR, "slDLSSDSetOptions", reinterpret_cast<void *&>(optionsFn)),
            "Import DLSSDSetOptions");
    logLine("DLSS Ray Reconstruction supported and initialized on the selected adapter.");
    auto feature = [&](sl::Feature f, const char *name, auto &fn) {
        slCheck(featureFn(f, name, reinterpret_cast<void *&>(fn)), name);
    };
    if (supportedFn(sl::kFeaturePCL, adapter) == sl::Result::eOk) {
        PFun_slPCLGetState *stateFn = nullptr;
        PFun_slPCLSetOptions *setFn = nullptr;
        feature(sl::kFeaturePCL, "slPCLSetMarker", markerFn);
        feature(sl::kFeaturePCL, "slPCLGetState", stateFn);
        feature(sl::kFeaturePCL, "slPCLSetOptions", setFn);
        sl::PCLOptions pcl{};
        pcl.idThread = GetCurrentThreadId();
        slCheck(setFn(pcl), "PCL options");
        sl::PCLState state{};
        slCheck(stateFn(state), "PCL state");
        pingMessage = state.statsWindowMessage;
    }
    if (supportedFn(sl::kFeatureReflex, adapter) == sl::Result::eOk) {
        PFun_slReflexGetState *stateFn = nullptr;
        feature(sl::kFeatureReflex, "slReflexSleep", sleepFn);
        feature(sl::kFeatureReflex, "slReflexSetOptions", reflexOptionsFn);
        feature(sl::kFeatureReflex, "slReflexGetState", stateFn);
        sl::ReflexState state{};
        slCheck(stateFn(state), "Reflex state");
        reflexAvailable = state.lowLatencyAvailable;
        sl::ReflexOptions reflex{};
        reflex.mode = reflexAvailable ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff;
        slCheck(reflexOptionsFn(reflex), "Reflex low latency options");
    }
    const auto support = supportedFn(sl::kFeatureDLSS_G, adapter);
    fgSupported = support == sl::Result::eOk && reflexAvailable && markerFn;
    if (fgSupported) {
        fgLoaded = true;
        importFrameGeneration();
        sl::DLSSGState state{};
        slCheck(fgStateFn(viewport, state, nullptr), "DLSS-FG capabilities");
        maxMultiplier = state.numFramesToGenerateMax + 1;
        minimumDimension = state.minWidthOrHeight;
        vsyncSupported = state.bIsVsyncSupportAvailable == sl::Boolean::eTrue;
        if (maxMultiplier < 2) {
            fgSupported = false;
            fgReason = "Driver reports no generated-frame capacity";
        }
    } else if (support == sl::Result::eErrorOSDisabledHWS)
        fgReason =
            "Enable Windows Hardware-accelerated GPU scheduling and restart to use DLSS Frame Generation";
    else if (!reflexAvailable || !markerFn)
        fgReason = "DLSS Frame Generation requires Streamline Reflex and PCL";
    else
        fgReason = "DLSS Frame Generation unsupported by this GPU, driver or OS (code " +
                   std::to_string(int(support)) + ")";
    logLine(fgSupported
                ? "DLSS Frame Generation supported; maximum multiplier " + std::to_string(maxMultiplier)
                : fgReason);
}
void Streamline::importFrameGeneration() {
    slCheck(featureFn(sl::kFeatureDLSS_G, "slDLSSGGetState", reinterpret_cast<void *&>(fgStateFn)),
            "DLSSGGetState import");
    slCheck(featureFn(sl::kFeatureDLSS_G, "slDLSSGSetOptions", reinterpret_cast<void *&>(fgOptionsFn)),
            "DLSSGSetOptions import");
}
void Streamline::loadFrameGeneration(bool enabled) {
    if (enabled && !fgSupported)
        throw std::runtime_error(fgReason);
    if (enabled == fgLoaded)
        return;
    slCheck(loadedFn(sl::kFeatureDLSS_G, enabled), "DLSS-FG plugin lifecycle");
    fgLoaded = enabled;
    fgEnabled = false;
    resumeAfterPresent = presentMarkers + 1;
    if (enabled)
        importFrameGeneration();
    else {
        fgStateFn = nullptr;
        fgOptionsFn = nullptr;
    }
}
void Streamline::setMultiplier(uint32_t value) {
    if (!value || (value != 1 && (!fgSupported || value > maxMultiplier)))
        throw std::runtime_error("Unsupported DLSS frame multiplier: " + fgReason);
    multiplier = value;
}
void Streamline::beginFrame(uint32_t index) {
    if (frameInProgress)
        throw std::runtime_error("Streamline frame already in progress");
    slCheck(tokenFn(currentToken, &index), "Shared Streamline frame token");
    frameInProgress = true;
    ++frameTokens;
    if (sleepFn)
        slCheck(sleepFn(*currentToken), "Reflex sleep");
    marker(sl::PCLMarker::eSimulationStart);
    if (pingPending)
        marker(sl::PCLMarker::ePCLatencyPing);
    if (clickPending)
        marker(sl::PCLMarker::eTriggerFlash);
    pingPending = clickPending = false;
}
void Streamline::marker(sl::PCLMarker value) {
    if (!frameInProgress)
        return;
    if (markerFn)
        slCheck(markerFn(value, *currentToken), "Streamline latency marker");
    if (value == sl::PCLMarker::eSimulationStart)
        ++simulationMarkers;
    if (value == sl::PCLMarker::eRenderSubmitStart)
        ++submissionMarkers;
    if (value == sl::PCLMarker::ePresentStart)
        ++presentMarkers;
}
void Streamline::inputMessage(UINT message, bool click) {
    const bool ping = pingMessage && message == pingMessage;
    if (frameInProgress) {
        if (ping)
            marker(sl::PCLMarker::ePCLatencyPing);
        if (click)
            marker(sl::PCLMarker::eTriggerFlash);
    } else {
        pingPending |= ping;
        clickPending |= click;
    }
}
void Streamline::clearFrameGenerationTags() {
    if (!currentToken)
        return;
    const sl::BufferType types[] = {sl::kBufferTypeDepth, sl::kBufferTypeMotionVectors,
                                    sl::kBufferTypeHUDLessColor, sl::kBufferTypeUIColorAndAlpha,
                                    sl::kBufferTypeBidirectionalDistortionField};
    sl::ResourceTag tags[6];
    for (int i = 0; i < 5; ++i)
        tags[i] = sl::ResourceTag(nullptr, types[i], sl::ResourceLifecycle::eValidUntilPresent);
    sl::Extent output{0, 0, width, height};
    tags[5] = sl::ResourceTag(nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent,
                              &output);
    slCheck(tagFn(*currentToken, viewport, tags, 6, nullptr), "Clear DLSS-FG tags");
    distortionTagged = false;
}
void Streamline::suspend() {
    if (fgLoaded && fgEnabled && fgOptionsFn) {
        fgOptions.mode = sl::DLSSGMode::eOff;
        fgOptions.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
        slCheck(fgOptionsFn(viewport, fgOptions), "Suspend DLSS Frame Generation");
        clearFrameGenerationTags();
        // Observe at least one non-interpolated Present between an Off update
        // and re-enabling, including resize messages arriving mid-simulation.
        resumeAfterPresent = presentMarkers + 1;
    }
    fgEnabled = false;
}
void Streamline::prepareFrame(bool gameFrame) {
    const bool enable = fgLoaded && multiplier > 1 && gameFrame && !fgStatus &&
                        presentMarkers >= resumeAfterPresent && width >= minimumDimension &&
                        height >= minimumDimension;
    if (!enable) {
        if (fgEnabled)
            suspend();
        return;
    }
    fgOptions = {};
    fgOptions.mode = sl::DLSSGMode::eOn;
    fgOptions.numFramesToGenerate = multiplier - 1;
    fgOptions.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    fgOptions.enableUserInterfaceRecomposition = sl::Boolean::eTrue;
    if (!fgEnabled)
        slCheck(fgOptionsFn(viewport, fgOptions), "Enable DLSS Frame Generation");
    fgEnabled = true;
}
void Streamline::tagFrameGeneration(ID3D12GraphicsCommandList *cmd, ID3D12Resource *depth,
                                    ID3D12Resource *motion, ID3D12Resource *hudless, ID3D12Resource *ui,
                                    ID3D12Resource *distortion) {
    if (!fgEnabled) {
        if (fgLoaded)
            clearFrameGenerationTags();
        return;
    }
    sl::Extent input{0, 0, renderWidth, renderHeight}, output{0, 0, width, height};
    ID3D12Resource *textures[] = {depth, motion, hudless, ui};
    const sl::BufferType types[] = {sl::kBufferTypeDepth, sl::kBufferTypeMotionVectors,
                                    sl::kBufferTypeHUDLessColor, sl::kBufferTypeUIColorAndAlpha};
    sl::Resource resources[4];
    sl::ResourceTag tags[6];
    for (int i = 0; i < 4; ++i) {
        resources[i] = sl::Resource(sl::ResourceType::eTex2d, textures[i],
                                    i < 2 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                          : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        tags[i] = sl::ResourceTag(&resources[i], types[i], sl::ResourceLifecycle::eValidUntilPresent,
                                  i < 2 ? &input : &output);
    }
    // Explicit full-backbuffer extent avoids ambiguous optional subrect state.
    tags[4] = sl::ResourceTag(nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent,
                              &output);
    sl::Resource distortionResource(sl::ResourceType::eTex2d, distortion,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    sl::Extent distortionExtent{0, 0, width, height};
    // Always submit the optional tag, including null in rectilinear mode, so a
    // cached fisheye map cannot affect frames after switching back.
    tags[5] = sl::ResourceTag(distortion ? &distortionResource : nullptr,
                              sl::kBufferTypeBidirectionalDistortionField,
                              sl::ResourceLifecycle::eValidUntilPresent, &distortionExtent);
    slCheck(tagFn(*currentToken, viewport, tags, 6, cmd), "DLSS-FG depth/motion/HUD/lens inputs");
    distortionTagged = distortion != nullptr;
    distortionFrames += distortionTagged ? 1 : 0;
}
void Streamline::endPresent(ID3D12CommandQueue *queue) {
    lastPresented = 1;
    if (fgLoaded) {
        sl::DLSSGState state{};
        slCheck(fgStateFn(viewport, state, nullptr), "DLSS-FG runtime state");
        // Protect reused input textures and CPU-side resize/free against FG's
        // asynchronous queue. The renderer signals/waits its own fence next.
        if (state.inputsProcessingCompletionFence &&
            FAILED(queue->Wait(static_cast<ID3D12Fence *>(state.inputsProcessingCompletionFence),
                               state.lastPresentInputsProcessingCompletionFenceValue)))
            throw std::runtime_error("DLSS-FG input completion wait failed");
        lastPresented = state.numFramesActuallyPresented;
        fgStatus = uint32_t(state.status);
        if (fgStatus) {
            fgReason = "DLSS-FG runtime status " + std::to_string(fgStatus);
            logLine(fgReason);
            suspend();
        }
    }
    presentedFrames += lastPresented;
    if (lastPresented > 1)
        interpolatedPresents += lastPresented - 1;
    frameInProgress = false; // Retain token only for null resource tags during resize/shutdown.
}
void Streamline::report(std::ostream &out) const {
    out << "{\"supported\":" << (fgSupported ? "true" : "false")
        << ",\"loaded\":" << (fgLoaded ? "true" : "false")
        << ",\"enabled\":" << (fgEnabled ? "true" : "false") << ",\"multiplier\":" << multiplier
        << ",\"maximumMultiplier\":" << maxMultiplier << ",\"minimumDimension\":" << minimumDimension
        << ",\"status\":" << fgStatus << ",\"reflex\":" << (reflexAvailable ? "true" : "false")
        << ",\"presentedFrames\":" << presentedFrames << ",\"extraPresents\":" << interpolatedPresents
        << ",\"distortionTagged\":" << (distortionTagged ? "true" : "false")
        << ",\"distortionFrames\":" << distortionFrames
        << ",\"frameTokens\":" << frameTokens << ",\"simulationMarkers\":" << simulationMarkers
        << ",\"submissionMarkers\":" << submissionMarkers << ",\"presentMarkers\":" << presentMarkers << "}";
}
void Streamline::free() {
    suspend();
    resumeAfterPresent = presentMarkers + 1;
    if (fgLoaded)
        slCheck(freeFn(sl::kFeatureDLSS_G, viewport), "Free DLSS-FG resources");
    if (active && optionsFn && evaluations)
        slCheck(freeFn(sl::kFeatureDLSS_RR, viewport), "slFreeResources");
}
void Streamline::shutdown() {
    if (active) {
        slCheck(shutdownFn(), "slShutdown");
        active = false;
    }
}
Streamline::~Streamline() {
    // Streamline shuts down before D3D objects, but its interposer must remain
    // loaded until the final swapchain/device/factory proxy has been released.
    if (active) {
        try {
            shutdown();
        } catch (const std::exception &e) {
            logLine(e.what());
        }
    }
    if (module)
        FreeLibrary(module);
}
std::pair<uint32_t, uint32_t> Streamline::configure(uint32_t w, uint32_t h, int quality) {
    width = w;
    height = h;
    options.mode = quality == 0   ? sl::DLSSMode::eMaxQuality
                   : quality == 1 ? sl::DLSSMode::eBalanced
                                  : sl::DLSSMode::eMaxPerformance;
    options.outputWidth = w;
    options.outputHeight = h;
    options.colorBuffersHDR = sl::Boolean::eTrue;
    options.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
    options.preExposure = 1;
    options.exposureScale = 1;
    sl::DLSSDOptimalSettings optimal{};
    slCheck(optimalFn(options, optimal), "DLSSD optimal resolution");
    renderWidth = optimal.optimalRenderWidth;
    renderHeight = optimal.optimalRenderHeight;
    if (!renderWidth || !renderHeight || renderWidth > w || renderHeight > h)
        throw std::runtime_error("DLSS returned an invalid input resolution");
    logLine("DLSS RR + upscaling: " + std::to_string(renderWidth) + "x" + std::to_string(renderHeight) +
            " -> " + std::to_string(w) + "x" + std::to_string(h));
    return {renderWidth, renderHeight};
}
void Streamline::evaluate(ID3D12GraphicsCommandList *cmd, const std::array<ID3D12Resource *, 8> &tex,
                          const Camera &cam, const Camera &prev, XMFLOAT2 jitter, bool reset,
                          uint32_t index) {
    const auto view = XMLoadFloat4x4(&cam.view), proj = XMLoadFloat4x4(&cam.projection),
               vp = XMLoadFloat4x4(&cam.viewProjection), old = XMLoadFloat4x4(&prev.viewProjection);
    options.worldToCameraView = matrix(view);
    options.cameraViewToWorld = matrix(XMMatrixInverse(nullptr, view));
    slCheck(optionsFn(viewport, options), "DLSSD options");
    if (!frameInProgress)
        throw std::runtime_error("RR needs the simulation frame token");
    (void)index;
    auto token = currentToken;
    sl::Constants c{};
    c.cameraViewToClip = matrix(proj);
    c.clipToCameraView = matrix(XMMatrixInverse(nullptr, proj));
    c.clipToLensClip = matrix(XMMatrixIdentity());
    c.clipToPrevClip = matrix(XMMatrixInverse(nullptr, vp) * old);
    c.prevClipToClip = matrix(XMMatrixInverse(nullptr, old) * vp);
    c.jitterOffset = {jitter.x, jitter.y};
    c.mvecScale = {1.f / renderWidth, 1.f / renderHeight};
    c.cameraPinholeOffset = {0, 0};
    c.cameraPos = {cam.position.x, cam.position.y, cam.position.z};
    c.cameraUp = {cam.up.x, cam.up.y, cam.up.z};
    c.cameraRight = {cam.right.x, cam.right.y, cam.right.z};
    c.cameraFwd = {cam.forward.x, cam.forward.y, cam.forward.z};
    c.cameraNear = .05f;
    c.cameraFar = 200;
    c.cameraFOV = 2 * std::atan(1.f / cam.projection._22);
    c.cameraAspectRatio = float(width) / height;
    c.depthInverted = sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.motionVectorsJittered = sl::Boolean::eFalse;
    c.motionVectorsDilated = sl::Boolean::eFalse;
    c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    slCheck(constantsFn(c, *token, viewport), "SL camera constants");
    sl::Extent input{0, 0, renderWidth, renderHeight}, output{0, 0, width, height};
    const sl::BufferType types[] = {sl::kBufferTypeScalingInputColor,
                                    sl::kBufferTypeMotionVectors,
                                    sl::kBufferTypeLinearDepth,
                                    sl::kBufferTypeNormalRoughness,
                                    sl::kBufferTypeAlbedo,
                                    sl::kBufferTypeSpecularAlbedo,
                                    sl::kBufferTypeSpecularHitDistance,
                                    sl::kBufferTypeScalingOutputColor};
    std::array<sl::Resource, 8> resources{};
    std::array<sl::ResourceTag, 8> tags{};
    for (int i = 0; i < 8; i++) {
        resources[i] = sl::Resource(sl::ResourceType::eTex2d, tex[i],
                                    i == 7 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                           : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        tags[i] = sl::ResourceTag(&resources[i], types[i], sl::ResourceLifecycle::eValidUntilPresent,
                                  i == 7 ? &output : &input);
    }
    slCheck(tagFn(*token, viewport, tags.data(), uint32_t(tags.size()), cmd), "SL reconstruction inputs");
    const sl::BaseStructure *inputs[] = {&viewport};
    slCheck(evaluateFn(sl::kFeatureDLSS_RR, *token, inputs, 1, cmd), "DLSS Ray Reconstruction evaluation");
    evaluations++;
}

} // namespace lab
