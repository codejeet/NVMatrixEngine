#include "hud.h"
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <windowsx.h>
// The shared UI backend only needs this HRESULT helper, not the old renderer implementation.
void hrCheck(HRESULT hr, const char *op) {
    if (FAILED(hr))
        throw std::runtime_error(std::string(op) + " failed (" + std::to_string(unsigned(hr)) + ")");
}
namespace lab {
Hud::Hud(ID3D12Device *device, HWND hwnd, const std::filesystem::path &folder, Game &g, ExperienceSettings &s,
         bool silent)
    : audio(folder / "assets/audio", silent), game(g), settings(s), window(hwnd),
      gpu(device, folder / "shaders/ui.hlsl") {
    system.SetWindow(hwnd);
    Rml::SetSystemInterface(&system);
    Rml::SetRenderInterface(&gpu);
    Rml::SetTextInputHandler(&ime);
    if (!Rml::Initialise())
        throw std::runtime_error("Lab RmlUi initialization failed");
    try {
        if (!Rml::LoadFontFace((folder / "ui/Poppins-Regular.ttf").string()))
            throw std::runtime_error("Lab font missing");
        context = Rml::CreateContext("spectral-playground", {1280, 720});
        if (!context)
            throw std::runtime_error("Lab RmlUi context failed");
        document = context->LoadDocument((folder / "ui/lab.rml").string());
        if (!document)
            throw std::runtime_error("Lab HUD missing");
        document->AddEventListener("click", this);
        document->AddEventListener("change", this);
        document->Show();
        if (settings.oceanLab) {
            auto grid = document->GetElementById("grid-resolution");
            grid->SetAttribute("min", "80"); grid->SetAttribute("max", "120"); grid->SetAttribute("step", "10");
        }
        if (settings.largeWaterLab) {
            auto grid = document->GetElementById("grid-resolution");
            grid->SetAttribute("min", "20");
            grid->SetAttribute("max", "32");
            grid->SetAttribute("step", "2");
        }
        if (settings.deepPool) {
            auto grid = document->GetElementById("grid-resolution");
            grid->SetAttribute("min", "50");
            grid->SetAttribute("max", "100");
            grid->SetAttribute("step", "2");
        }
        for (auto [id, value] : {std::pair{"lens-fov", settings.lens.diagonalDegrees},
                                 {"particle-limit", float(settings.particleCapacity / 1000)},
                                 {"grid-resolution", settings.cellSize * 100},
                                 {"simulation-rate", settings.simulationHz}})
            if (auto control = dynamic_cast<Rml::ElementFormControl *>(document->GetElementById(id)))
                control->SetValue(std::to_string(value));
    } catch (...) {
        Rml::Shutdown();
        throw;
    }
}
Hud::~Hud() {
    document->RemoveEventListener("click", this);
    document->RemoveEventListener("change", this);
    document->Close();
    Rml::RemoveContext("spectral-playground");
    Rml::Shutdown();
    Rml::SetRenderInterface(nullptr);
    Rml::SetSystemInterface(nullptr);
    Rml::SetTextInputHandler(nullptr);
}
void Hud::text(const char *id, const std::string &s) {
    if (auto e = document->GetElementById(id))
        if (e->GetInnerRML() != s)
            e->SetInnerRML(s);
}
void Hud::visible(const char *id, bool show) {
    auto [it, inserted] = visibility.try_emplace(id, show);
    if (!inserted && it->second == show)
        return;
    it->second = show;
    document->GetElementById(id)->SetProperty("display", show ? "block" : "none");
}
POINT Hud::wallWaterButtonPoint() const {
    auto e = document->GetElementById("wall-water");
    auto p = e->GetAbsoluteOffset();
    auto size = e->GetBox().GetSize();
    return {LONG(p.x + size.x * .5f), LONG(p.y + size.y * .5f)};
}
bool Hud::input(UINT msg, WPARAM w, LPARAM l) {
    if (game.paused || game.won) {
        RmlWin32::WindowProcedure(context, ime, window, msg, w, l);
        return true;
    }
    if (fluidRoom && msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) {
        POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
            ScreenToClient(window, &point);
        auto element = context->GetElementAtPoint({float(point.x), float(point.y)});
        bool over = false;
        for (auto e = element; e; e = e->GetParentNode())
            if (e->GetId() == "wall-controls")
                over = true;
        const bool consume = over || waterPointerDown;
        if (msg == WM_LBUTTONDOWN && over)
            waterPointerDown = true;
        RmlWin32::WindowProcedure(context, ime, window, msg, w, l);
        if (msg == WM_LBUTTONUP)
            waterPointerDown = false;
        return consume;
    }
    return false;
}
void Hud::testClick(const char *id) {
    auto element = document->GetElementById(id);
    if (!element)
        throw std::runtime_error(std::string("Missing UI control: ") + id);
    element->DispatchEvent("click", {});
}
void Hud::testValue(const char *id, float value) {
    auto control = dynamic_cast<Rml::ElementFormControl *>(document->GetElementById(id));
    if (!control)
        throw std::runtime_error(std::string("Missing UI range: ") + id);
    control->SetValue(std::to_string(value));
    control->DispatchEvent("change", {});
}
void Hud::ProcessEvent(Rml::Event &event) {
    auto e = event.GetTargetElement();
    if (event.GetType() == "change" && e->GetId() != "music") {
        if (auto control = dynamic_cast<Rml::ElementFormControl *>(e)) {
            const float value = std::stof(control->GetValue());
            if (e->GetId() == "lens-fov")
                settings.lens.diagonalDegrees = std::clamp(value, 90.f, 160.f);
            if (e->GetId() == "particle-limit")
                settings.particleCapacity = uint32_t(std::clamp(value, 100.f, 1000.f)) * 1000;
            if (e->GetId() == "grid-resolution")
                settings.cellSize =
                    std::clamp(value, settings.oceanLab ? 80.f : settings.deepPool ? 50.f : (settings.largeWaterLab ? 20.f : 10.f),
                               settings.oceanLab ? 120.f : settings.deepPool ? 100.f : (settings.largeWaterLab ? 32.f : 24.f)) *
                    .01f;
            if (e->GetId() == "simulation-rate")
                settings.simulationHz = std::clamp(value, 60.f, 180.f);
        }
        return;
    }
    if (event.GetType() == "change" && e->GetId() == "music") {
        if (auto control = dynamic_cast<Rml::ElementFormControl *>(e))
            musicVolume = std::clamp(std::stof(control->GetValue()) / 100, 0.f, 1.f);
        return;
    }
    while (e && !e->HasAttribute("data-action"))
        e = e->GetParentNode();
    if (!e || event.GetType() != "click")
        return;
    auto action = e->GetAttribute<Rml::String>("data-action", "");
    if (action == "dlss-quality")
        requestedDlssQuality = e->GetAttribute<int>("data-quality", 0);
    if (action == "environment")
        settings.environment = (settings.environment + 1) % (settings.oceanLab ? 2 : 4);
    if (action == "flashlight")
        settings.flashlight = !settings.flashlight;
    if (action == "lens")
        settings.lens.fisheye = !settings.lens.fisheye;
    if (action == "view")
        settings.firstPerson = !settings.firstPerson;
    if (action == "ball-density" && fluidRoom && game.boatBody >= 0)
        settings.ballFloats = !settings.ballFloats;
    if (action == "apply-water" && fluidRoom)
        settings.rebuildWater = true;
    if (action == "wall-water" && fluidRoom && !waterFull)
        requestedWallWater = true;
    if (action == "resume")
        game.paused = false;
    if (action == "restart") {
        game.load(0);
        game.paused = false;
    }
    if (action == "quit")
        quit = true;
    if (action == "frame-gen" && maxFrameMultiplier >= 2)
        requestedFrameMultiplier =
            frameMultiplier == 1
                ? 2
                : (frameMultiplier < std::min(4u, maxFrameMultiplier) ? int(frameMultiplier + 1) : 1);
    game.clearInput();
    audio.play(Sound::Click);
}
void Hud::update(float dt, float watts) {
    audio.update(dt, musicVolume, .45f, GetForegroundWindow() == window);
    if (game.jumps > jumps)
        audio.play(Sound::Jump);
    if (game.throws > throws)
        audio.play(Sound::Throw);
    if (game.held >= 0 && game.held != held)
        audio.play(Sound::Grab);
    if (game.tuneMoves > moves)
        audio.play(Sound::Tick);
    if (game.tuneTurns > turns)
        audio.play(Sound::Rotate);
    if (game.gateOpen && !gate)
        audio.play(Sound::Unlock);
    if (game.won && !won)
        audio.play(Sound::Victory);
    jumps = game.jumps;
    throws = game.throws;
    held = game.held;
    moves = game.tuneMoves;
    turns = game.tuneTurns;
    gate = game.gateOpen;
    won = game.won;
    // Separate measured render and output throughput, including CPU/simulation
    // and display pacing. Start a fresh window when the rendering mode changes.
    const auto mode = std::to_string(dlssQuality) + "/" + std::to_string(frameMultiplier) +
                      (frameGenActive ? "/on/" : "/off/") + std::to_string(outputWidth) + "x" +
                      std::to_string(outputHeight);
    if (rateMode != mode) {
        rateMode = mode;
        frameRates = {};
        text("render-fps", "Render: -- FPS");
        text("output-fps", "DLSS output: -- FPS");
    }
    const double nowMs = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now().time_since_epoch()).count();
    if (frameRates.sample(nowMs, renderedFrames, presentedFrames)) {
        text("render-fps", "Render: " + std::to_string(int(std::round(frameRates.renderFps))) + " FPS");
        text("output-fps", "DLSS output: " + std::to_string(int(std::round(frameRates.outputFps))) + " FPS");
    }
    std::string fgState;
    if (maxFrameMultiplier < 2)
        fgState = "FG unavailable";
    else if (frameMultiplier == 1)
        fgState = "FG off";
    else if (frameGenStatus)
        fgState = "FG error " + std::to_string(frameGenStatus);
    else if (game.paused || game.won)
        fgState = "FG paused: menu";
    else if (outputWidth < frameGenMinimumDimension || outputHeight < frameGenMinimumDimension)
        fgState = "FG paused: resolution too low";
    else if (!frameGenActive)
        fgState = "FG paused / preparing";
    else
        fgState = "FG " + std::to_string(frameMultiplier) + "x: " +
                  (!frameRates.ready ? "measuring" : frameRates.outputFps > frameRates.renderFps
                                                        ? "generating" : "no extra frames reported");
    const char *qualityNames[] = {"Quality", "Balanced", "Performance"};
    text("dlss-status", std::string(renderedFrames ? "RR " : "RR starting: ") +
                            qualityNames[dlssQuality] + " · " + fgState +
                            (settings.lens.fisheye ? " · fisheye" : ""));
    text("dlss-quality-info", std::string("DLSS RR: ") + qualityNames[dlssQuality] + " · " +
                                  std::to_string(renderWidth) + " × " + std::to_string(renderHeight) +
                                  " to " + std::to_string(outputWidth) + " × " + std::to_string(outputHeight));
    if (selectedDlssQuality != dlssQuality) {
        const char *ids[] = {"dlss-quality", "dlss-balanced", "dlss-performance"};
        for (int i = 0; i < 3; ++i)
            document->GetElementById(ids[i])->SetClass("selected", i == dlssQuality);
        selectedDlssQuality = dlssQuality;
    }
    visible("pause", game.paused || game.won);
    const char *environments[] = {"Neon night", "Single overhead light", "White studio", "Blackout"};
    text("environment", settings.oceanLab ? (settings.environment ? "Lighting: starry night [Y]" : "Lighting: daylight [Y]")
                                         : std::string("Lighting: ") + environments[settings.environment % 4]);
    text("ocean-time", settings.environment ? "SWITCH TO DAY  [Y]" : "SWITCH TO NIGHT  [Y]");
    visible("ocean-controls", settings.oceanLab && !game.paused);
    text("flashlight", settings.flashlight ? "Ball flashlight: On" : "Ball flashlight: Off");
    text("lens", settings.lens.fisheye ? "Lens: equisolid fisheye" : "Lens: rectilinear");
    text("view", settings.firstPerson ? "View: first person [Tab]" : "View: orbit [Tab]");
    visible("ball-settings", fluidRoom && game.boatBody >= 0);
    text("ball-density", settings.ballFloats ? "Glass ball: Float (hollow)" : "Glass ball: Sink (solid)");
    text("ball-density-info", std::to_string(int(std::round(game.ballDensity()))) + " kg/m³ · " +
                                  std::to_string(int(std::round(game.ballMass()))) +
                                  " kg. Applies immediately; also changes the boat's passenger load.");
    text("lens-value",
         "Diagonal field of view: " + std::to_string(int(settings.lens.diagonalDegrees)) + " degrees");
    text("capacity-value", "Particle capacity: " + std::to_string(settings.particleCapacity / 1000) + "k");
    visible("capacity-value", !hamiltonianWater);
    visible("particle-limit", !hamiltonianWater);
    text("water-quality-description", hamiltonianWater
        ? "Finer cells and higher rates cost GPU time. Applying resets the water."
        : "Capacity reserves room for the inlet; it does not spawn particles. Finer cells and higher rates cost GPU time. Applying resets the water.");
    text("grid-value", "MAC cell size: " + std::to_string(int(std::round(settings.cellSize * 100))) +
                           " cm (smaller = finer)");
    text("rate-value", "Simulation rate: " + std::to_string(int(settings.simulationHz)) + " Hz");
    visible("water-settings", fluidRoom);
    const bool compact = game.firstPerson || game.piloting;
    visible("wall-controls", fluidRoom && !settings.oceanLab && !game.won && !compact);
    visible("receiver", !settings.oceanLab && !compact);
    text("wall-water", waterFull ? "CAPACITY FULL — B to reset"
                                 : (wallWater ? "STOP WALL WATER  [T]" : "SPEW WALL WATER  [T]"));
    text("water-state", std::string(waterFull ? "Safety valve closed. No particles discarded."
                                  : (wallWater ? "Inlet open · flowing into the room"
                                               : "Wall valve closed · click or press T")) +
                        (hamiltonianWater ? " · " + waterFillStatus : ""));
    visible("details", game.ui || game.hint);
    text("pt-mode", rendererStatus);
    visible("tuning", game.tuning >= 0);
    visible("objective", game.tuning < 0 && !compact);
    text("pause-title", game.won ? "Spectrum restored" : "Take a breath");
    text("frame-gen",
         maxFrameMultiplier < 2
             ? "DLSS Frame Generation unavailable"
             : (frameMultiplier == 1 ? "DLSS Frame Generation: Off"
                                     : "DLSS Frame Generation: " + std::to_string(frameMultiplier) + "x"));
    text("frame-gen-info",
         maxFrameMultiplier < 2 || frameGenStatus
             ? frameGenReason
             : "F8: 2x / Off. Frame Generation resumes after closing this menu. Output FPS counts actual "
               "presented frames, including generated frames. Both lens modes support Frame Generation; "
               "RR upscaling alone does not add frames.");
    visible("resume", !game.won);
    text("charge",
         game.gateOpen ? "PORTAL OPEN" : "RECEIVER  /  " + std::to_string(int(game.charge * 100)) + "%");
    int charge = int(game.charge * 1000);
    if (charge != chargeTenths) {
        document->GetElementById("fill")->SetProperty("width", std::to_string(charge / 10.f) + "%");
        chargeTenths = charge;
    }
    readingTime += dt;
    if (readingTime >= .1f) {
        std::ostringstream reading;
        reading << std::fixed << std::setprecision(3) << watts << " W / 0.100 W  ·  510–570 nm";
        text("power", reading.str());
        text("fluid", fluidStatus);
        readingTime = 0;
    }
    text("hint", settings.oceanLab ? game.level().hint : hamiltonianWater
                     ? "T fills the room. Throw objects or pilot the boat to send waves through it. B drains/resets the water; P pauses it."
                     : game.level().hint);
    const bool fluidLab = !fluidStatus.empty();
    text("objective-title",
         settings.oceanLab ? "Ocean Island."
         : largeWaterLab ? "Large Water Lab."
         : hamiltonianWater ? "Hamiltonian waves."
         : fluidRoom ? "Flood the chamber." : (fluidLab ? "GPU liquid lab." : "Bend the night."));
    text("objective-description",
         settings.oceanLab ? "An island, open water, and a changing sky."
         : largeWaterLab ? "24 × 28 metre Water Lab · 1.5 metre starting depth · boat and caustics."
         : hamiltonianWater ? "Nonlinear waves · boat wakes · ray-traced water and caustics."
         : fluidRoom  ? "Room-wide water · wall inlet · physically traced foam and bubbles."
         : fluidLab ? "GPU APIC / FLIP. Reconstructed water bends light, lasers and caustics."
                    : "Bring the green spectrum into the marked receiver.");
    text("shortcuts",
         settings.oceanLab ? "Y: day / night · I: overview · Tab: view · Space / Ctrl: swim up / down · P: pause water · Esc: settings"
         : compact                     ? "Tab: view · Space / Ctrl: swim up / down · Right drag: look · Esc: settings"
         : !complexityStatus.empty() ? complexityStatus
         : hamiltonianWater ? "T: fill water · P: pause water · .: step · B: drain/reset · U: details · F8: Frame Gen"
         : fluidRoom ? "T: wall water · P: pause water · B: drain/reset · U: details · F8: Frame Gen"
         : fluidLab  ? "I: inspect pool · P: pause · .: step · B: reset · V/G: grid · N: surface · U: details"
                     : "L: laser color · U / H: hints &amp; details · Esc: pause · R: reset");
    text("water-description", settings.oceanLab
        ? "Explore the beach and board the boat from the pier. The offshore depth is six metres. T operates the pier inlet; B resets the water."
        : hamiltonianWater
        ? "Nonlinear waves and local splashes share the surface that bends light and casts caustics."
        : "GPU liquid uses the actual animated surface for refraction and caustics. Secondary whitewater follows the flow; the wall inlet stops safely at capacity.");
    text("laser", "L: laser wavelength  /  " + std::to_string(int(laserWavelength)) + " nm  /  1.5 W each");
    std::string prompt =
        game.tuning >= 0
            ? (game.gateOpen
                   ? "Portal open! Enter / F: return to your ball, then roll through the cyan portal."
                   : "Drag to move  ·  Wheel / Q C to rotate  ·  Shift: fine  ·  Enter / F: save  ·  Esc: "
                     "cancel")
        : game.gateOpen  ? "Follow the cyan portal behind the source. Roll through to finish."
        : game.held >= 0 ? "Left click / X: throw  ·  E: release  ·  F: lock prism into its cradle"
        : game.nearbyOptic() > 0
            ? "F: lock prism + top-down tuning  ·  E: pick up"
            : "WASD: roll  ·  Space: jump  ·  E: grab  ·  Right drag: orbit  ·  Wheel: zoom";
    if (settings.oceanLab)
        prompt = game.held >= 0 ? "Left click / X: throw  ·  E: release"
                               : "WASD: move  ·  Space: jump / swim up  ·  Ctrl: dive  ·  E: grab  ·  Right drag: look";
    text("context",
         nearbyValve && game.tuning < 0 ? (settings.oceanLab ? "E: operate pier outlet  ·  T: remote outlet  ·  B: reset water"
                                                           : "E: press wall valve  ·  T: remote valve  ·  B: drain/reset")
         : game.piloting
             ? "W/S: throttle / reverse · A/D: steer · E: leave boat · Tab: first person · Right drag: look"
         : game.nearBoat() ? "E: board the boat · Tab: first person · Right drag: look"
                           : prompt);
    if (game.tuning >= 0) {
        auto p = game.opticPosition();
        std::ostringstream s;
        s << std::fixed << std::setprecision(2) << "X " << p.x << " m   Z " << p.z << " m   Yaw "
          << game.opticYaw() * 180 / XM_PI << "°";
        text("pose", s.str());
    }
}
void Hud::render(ID3D12GraphicsCommandList *cmd, int width, int height) {
    if (uiWidth != width || uiHeight != height) {
        context->SetDimensions({width, height});
        uiWidth = width;
        uiHeight = height;
    }
    gpu.begin(cmd, width, height);
    context->Update();
    context->Render();
}
} // namespace lab
