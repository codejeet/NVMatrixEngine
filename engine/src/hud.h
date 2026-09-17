#pragma once
#include "gameplay.h"
#include "ui_renderer.h"
#include "audio.h"
#include "experience.h"
#include "frame_rates.h"
#include <RmlUi/Core.h>
#include <RmlUi_Platform_Win32.h>
#include <unordered_map>
namespace lab {
class Hud final : public Rml::EventListener {
  public:
    Hud(ID3D12Device *, HWND, const std::filesystem::path &, Game &, ExperienceSettings &, bool silent);
    ~Hud();
    bool input(UINT, WPARAM, LPARAM);
    void update(float dt, float watts);
    void render(ID3D12GraphicsCommandList *, int width, int height);
    std::array<uint64_t, 4> geometryUploadStats() const {
        return gpu.geometryUploadStats();
    }
    void ProcessEvent(Rml::Event &) override;
    POINT wallWaterButtonPoint() const;
    void testClick(const char *id);
    void testValue(const char *id, float value);
    bool quit = false;
    float musicVolume = .15f;
    float laserWavelength = 532;
    std::string fluidStatus;
    std::string complexityStatus;
    std::string rendererStatus;
    bool fluidRoom = false, wallWater = false, waterFull = false, nearbyValve = false;
    bool requestedWallWater = false;
    uint32_t frameMultiplier = 1, maxFrameMultiplier = 1;
    uint64_t renderedFrames = 0, presentedFrames = 0;
    uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
    uint32_t frameGenStatus = 0, frameGenMinimumDimension = 0;
    int dlssQuality = 0, requestedDlssQuality = -1;
    bool frameGenActive = false;
    int requestedFrameMultiplier = -1;
    std::string frameGenReason;
    Audio audio;

  private:
    Game &game;
    ExperienceSettings &settings;
    HWND window;
    UiRenderer gpu;
    SystemInterface_Win32 system;
    TextInputMethodEditor_Win32 ime;
    Rml::Context *context = nullptr;
    Rml::ElementDocument *document = nullptr;
    int jumps = 0, throws = 0, held = -1;
    uint64_t moves = 0, turns = 0;
    bool gate = false, won = false;
    bool waterPointerDown = false;
    std::unordered_map<std::string, bool> visibility;
    int chargeTenths = -1, uiWidth = 1280, uiHeight = 720;
    FrameRates frameRates;
    std::string rateMode;
    int selectedDlssQuality = -1;
    float readingTime = 1;
    void text(const char *, const std::string &);
    void visible(const char *, bool);
};
} // namespace lab
