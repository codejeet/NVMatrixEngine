#include "renderer.h"
#include "hud.h"
#include "orbit.h"
#include "startup.h"
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <string_view>
#include <iomanip>
#include <sstream>
namespace {
bool running = true, dragging = false, automation = false;
bool tuningDrag = false, injected = false;
bool rawMouse = false, relativeMouse = true;
lab::OrbitInput orbitInput;
Game *activeGame = nullptr;
lab::Hud *activeHud = nullptr;
lab::Renderer *activeRenderer = nullptr;
lab::StartupScreen *activeStartup = nullptr;
uint32_t viewWidth = 1280, viewHeight = 720;
XMFLOAT3 dragPoint{};
bool onPlane(int x, int y, XMFLOAT3 &point) {
    if (!activeGame || !viewHeight)
        return false;
    // Pick against the camera the player actually saw, without advancing its
    // collision recovery a second time while processing pointer messages.
    auto c = activeRenderer && activeRenderer->frame ? activeRenderer->viewCamera()
                                                     : activeGame->camera(float(viewWidth) / viewHeight);
    float sx = (2.f * x / viewWidth - 1) * float(viewWidth) / viewHeight * std::tan(XM_PI / 6);
    float sy = (1 - 2.f * y / viewHeight) * std::tan(XM_PI / 6);
    XMFLOAT3 ray{c.forward.x + c.right.x * sx + c.up.x * sy, c.forward.y + c.right.y * sx + c.up.y * sy,
                 c.forward.z + c.right.z * sx + c.up.z * sy};
    if (activeRenderer && activeRenderer->frame)
        ray = activeRenderer->displayRay(2.f * x / viewWidth - 1, 1 - 2.f * y / viewHeight);
    if (std::abs(ray.y) < .001f)
        return false;
    float t = (activeGame->opticPosition().y - c.position.y) / ray.y;
    if (t < 0 || t > 100)
        return false;
    point = {c.position.x + ray.x * t, activeGame->opticPosition().y, c.position.z + ray.z * t};
    return true;
}
int mouseX = 0, mouseY = 0;
float azimuth = .48f, elevation = .40f, angle = DirectX::XM_PI / 6;
uint32_t pendingWidth = 0, pendingHeight = 0;
LRESULT CALLBACK windowProc(HWND window, UINT msg, WPARAM w, LPARAM l) {
    LRESULT startupResult = 0;
    if (activeStartup && activeStartup->message(msg, w, l, startupResult))
        return startupResult;
    // F10 is delivered as a system key by Windows; retain Alt/menu shortcuts.
    if (w == VK_F10 && !(l & (1LL << 29)) && (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP))
        msg = msg == WM_SYSKEYDOWN ? WM_KEYDOWN : WM_KEYUP;
    if (activeRenderer)
        activeRenderer->notifyInput(msg, msg == WM_LBUTTONDOWN);
    if (msg == WM_INPUT) {
        RAWINPUT input{};
        UINT size = sizeof(input);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, &input, &size,
                            sizeof(RAWINPUTHEADER)) != UINT(-1) &&
            input.header.dwType == RIM_TYPEMOUSE) {
            relativeMouse = !(input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE);
            if (!automation && relativeMouse && dragging &&
                (!activeGame || (!activeGame->paused && !activeGame->won && activeGame->tuning < 0)))
                orbitInput.add(float(input.data.mouse.lLastX), float(input.data.mouse.lLastY));
        }
        // Foreground RAWINPUT handles require default processing for OS cleanup.
        return DefWindowProcW(window, msg, w, l);
    }
    // Desktop mouse/wheel traffic must never mutate an automated optical fixture.
    if (automation && !injected &&
        ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_KEYDOWN || msg == WM_KEYUP ||
         msg == WM_CHAR))
        return 0;
    bool uiInput = activeHud && activeHud->input(msg, w, l);
    switch (msg) {
    case WM_CLOSE:
        if (activeRenderer)
            activeRenderer->suspendFrameGeneration();
        running = false;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (activeRenderer)
            activeRenderer->suspendFrameGeneration();
        if (w != SIZE_MINIMIZED) {
            pendingWidth = LOWORD(l);
            pendingHeight = HIWORD(l);
            viewWidth = pendingWidth;
            viewHeight = pendingHeight;
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (activeGame) {
            if (uiInput || activeGame->paused || activeGame->won)
                return 0;
            if (msg == WM_LBUTTONDOWN && activeGame->tuning >= 0) {
                tuningDrag =
                    activeGame->tuningReady() && onPlane(GET_X_LPARAM(l), GET_Y_LPARAM(l), dragPoint);
                if (tuningDrag)
                    SetCapture(window);
                return 0;
            }
            if (msg == WM_LBUTTONDOWN && activeGame->held >= 0) {
                activeGame->throwHeld();
                return 0;
            }
            if (activeGame->tuning >= 0)
                return 0;
        }
        dragging = true;
        mouseX = GET_X_LPARAM(l);
        mouseY = GET_Y_LPARAM(l);
        SetCapture(window);
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        dragging = false;
        tuningDrag = false;
        ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        dragging = false;
        tuningDrag = false;
        return 0;
    case WM_KILLFOCUS:
        if (activeRenderer)
            activeRenderer->suspendFrameGeneration();
        orbitInput.clear();
        if (activeGame && !automation) {
            activeGame->clearInput();
            activeGame->paused = true;
        }
        dragging = tuningDrag = false;
        if (GetCapture() == window)
            ReleaseCapture();
        return 0;
    case WM_ENTERSIZEMOVE:
        if (activeRenderer)
            activeRenderer->suspendFrameGeneration();
        return 0;
    case WM_MOUSEMOVE:
        if (tuningDrag && activeGame && !activeGame->paused) {
            XMFLOAT3 p;
            if (onPlane(GET_X_LPARAM(l), GET_Y_LPARAM(l), p)) {
                float scale = activeGame->input.fine ? .15f : 1;
                activeGame->tuneMove((p.x - dragPoint.x) * scale, (p.z - dragPoint.z) * scale);
                dragPoint = p;
            }
            return 0;
        }
        if (dragging) {
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            // Keep legacy coordinates for absolute devices/registration failure
            // and deterministic injected tests. Never double-count raw motion.
            if (!rawMouse || !relativeMouse || injected)
                orbitInput.add(float(x - mouseX), float(y - mouseY));
            mouseX = x;
            mouseY = y;
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (activeGame) {
            if (uiInput || activeGame->paused)
                return 0;
            float wheel = GET_WHEEL_DELTA_WPARAM(w) / float(WHEEL_DELTA);
            if (activeGame->tuning >= 0)
                activeGame->tuneRotate(wheel * (activeGame->input.fine ? .0015f : .01f));
            else
                activeGame->distance = std::clamp(activeGame->distance - wheel * .5f, 3.f, 14.f);
            return 0;
        }
        angle += GET_WHEEL_DELTA_WPARAM(w) / float(WHEEL_DELTA) * .01f;
        return 0;
    case WM_KEYDOWN:
    case WM_KEYUP:
        if (activeGame) {
            auto &g = *activeGame;
            bool down = msg == WM_KEYDOWN, once = down && !(l & (1LL << 30));
            if (once && activeRenderer && activeRenderer->hasOpticalImportance() &&
                (w == VK_F4 || w == VK_F5)) {
                if (w == VK_F4)
                    activeRenderer->cycleOpticalDebug();
                else
                    activeRenderer->toggleOpticalFreeze();
                return 0;
            }
            if (once && activeRenderer && activeRenderer->fluidComplexity && (w == VK_F6 || w == VK_F7)) {
                auto &complexity = *activeRenderer->fluidComplexity;
                if (w == VK_F6)
                    complexity.debugMode = (complexity.debugMode + 1) % 11;
                else
                    complexity.frozen = !complexity.frozen;
                return 0;
            }
            if (once && w == VK_F8 && activeHud && activeRenderer) {
                const auto &state = activeRenderer->streamlineState();
                if (state.fgSupported)
                    activeHud->requestedFrameMultiplier = state.multiplier > 1 ? 1 : 2;
                return 0;
            }
            if (once && w == VK_ESCAPE) {
                if (g.tuning >= 0)
                    g.endTuning(true);
                else if (!g.won)
                    g.paused = !g.paused;
                g.clearInput();
                dragging = tuningDrag = false;
                ReleaseCapture();
                return 0;
            }
            if (once && w == 'R') {
                g.load(0);
                g.paused = false;
                return 0;
            }
            if (g.paused || g.won)
                return 0;
            switch (w) {
            case VK_TAB:
                if (once && activeRenderer)
                    activeRenderer->experience.firstPerson = !activeRenderer->experience.firstPerson;
                break;
            case 'Y':
                if (once && activeRenderer && activeRenderer->experience.oceanLab)
                    activeRenderer->experience.environment = 1 - activeRenderer->experience.environment;
                break;
            case 'W':
                g.input.forward = down;
                break;
            case 'S':
                g.input.back = down;
                break;
            case 'A':
                g.input.left = down;
                break;
            case 'D':
                g.input.right = down;
                break;
            case VK_SPACE:
                g.input.ascend = down;
                if (once)
                    g.input.jump = true;
                break;
            case VK_SHIFT:
                g.input.fine = down;
                break;
            case VK_CONTROL:
                g.input.dive = down;
                break;
            case 'Q':
                g.input.turnLeft = down;
                break;
            case 'C':
                g.input.turnRight = down;
                break;
            case 'E':
                if (once) {
                    if (g.piloting || g.nearBoat()) {
                        g.toggleBoat();
                    } else if (activeRenderer && activeRenderer->nearWallValve(g)) {
                        activeRenderer->toggleWallWater();
                        if (activeHud)
                            activeHud->audio.play(Sound::Click);
                    } else
                        g.grab();
                }
                break;
            case 'T':
                if (once && activeRenderer) {
                    activeRenderer->toggleWallWater();
                    if (activeHud && activeRenderer->roomLiquid())
                        activeHud->audio.play(Sound::Click);
                }
                break;
            case 'X':
                if (once)
                    g.throwHeld();
                break;
            case 'F':
            case VK_RETURN:
                if (once) {
                    if (g.tuning >= 0)
                        g.endTuning();
                    else if (g.beginTuning() && activeHud)
                        activeHud->audio.play(Sound::Dock);
                }
                break;
            case 'U':
                if (once)
                    g.ui = !g.ui;
                break;
            case 'H':
                if (once)
                    g.hint = !g.hint;
                break;
            case 'L':
                if (once && activeRenderer) {
                    auto &nm = activeRenderer->laserWavelength;
                    nm = nm == 532 ? 450.f : (nm == 450 ? 638.f : 532.f);
                    if (activeHud)
                        activeHud->audio.play(Sound::Click);
                }
                break;
            case 'P':
                if (once && activeRenderer && activeRenderer->fluid)
                    activeRenderer->fluid->paused = !activeRenderer->fluid->paused;
                break;
            case VK_F9:
                if (once && activeRenderer && activeRenderer->fluid && activeRenderer->fluid->bulk)
                    activeRenderer->fluid->bulk->debugMode = (activeRenderer->fluid->bulk->debugMode + 1) % 3;
                break;
            case 'K':
                if (once && activeRenderer && activeRenderer->fluid && activeRenderer->fluid->cutCells)
                    activeRenderer->fluid->cutCells->debugVisible =
                        !activeRenderer->fluid->cutCells->debugVisible;
                break;
            case VK_F12:
                if (once && activeRenderer && activeRenderer->fluid && activeRenderer->fluid->work) {
                    auto &f = *activeRenderer->fluid;
                    f.debugMode = f.debugMode == 7 ? 0 : 7;
                    f.debugVisible = true;
                }
                break;
            case VK_F10:
                if (once && activeRenderer && activeRenderer->fluid && activeRenderer->fluid->interior)
                    activeRenderer->fluid->interior->forcedFine =
                        !activeRenderer->fluid->interior->forcedFine;
                if (once && activeRenderer && activeRenderer->fluid && activeRenderer->fluid->mac)
                    activeRenderer->fluid->mac->forcedFine = !activeRenderer->fluid->mac->forcedFine;
                break;
            case VK_F11:
                if (once && activeRenderer && activeRenderer->fluid && activeRenderer->fluid->mac)
                    activeRenderer->fluid->mac->debugVisible = !activeRenderer->fluid->mac->debugVisible;
                break;
            case 'I':
                if (once && activeRenderer)
                    activeRenderer->toggleFluidView();
                break;
            case 'N':
                if (once && activeRenderer)
                    activeRenderer->cycleFluidSurfaceDebug();
                break;
            case VK_OEM_PERIOD:
                if (once && activeRenderer && activeRenderer->fluid)
                    activeRenderer->fluid->requestStep();
                break;
            case 'V':
                if (once && activeRenderer && activeRenderer->fluid)
                    activeRenderer->fluid->debugVisible = !activeRenderer->fluid->debugVisible;
                break;
            case 'G':
                if (once && activeRenderer && activeRenderer->fluid)
                    activeRenderer->fluid->debugMode =
                        (activeRenderer->fluid->debugMode + 1) %
                        (activeRenderer->fluid->interior ? 7 : (activeRenderer->fluid->resampling ? 6 : 5));
                break;
            case 'B':
                if (once && activeRenderer && activeRenderer->fluid)
                    activeRenderer->fluid->requestReset();
                break;
            }
            return 0;
        }
        if (msg == WM_KEYUP)
            return 0;
        if (w == VK_ESCAPE)
            running = false;
        if (w == 'R')
            angle = DirectX::XM_PI / 6;
        return 0;
    }
    return DefWindowProcW(window, msg, w, l);
}
uint32_t number(const std::string &text, uint32_t lo, uint32_t hi) {
    size_t end = 0;
    auto n = std::stoul(text, &end);
    if (end != text.size() || n < lo || n > hi)
        throw std::runtime_error("Numeric argument out of range: " + text);
    return uint32_t(n);
}
std::filesystem::path executableFolder() {
    // argv[0] can be only "NVMatrixFluidLab.exe" (cmd START), or a relative path.
    // Resolve the loaded module, never infer asset locations from the command line/cwd.
    std::wstring filename(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, filename.data(), DWORD(filename.size()));
    if (!length || length >= filename.size())
        throw std::runtime_error("Cannot resolve the lab executable's full path (Windows error " +
                                 std::to_string(GetLastError()) + ")");
    filename.resize(length);
    return std::filesystem::path(filename).parent_path();
}
struct CommandLineDeleter {
    void operator()(wchar_t **value) const {
        LocalFree(value);
    }
};
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const auto applicationStart = std::chrono::steady_clock::now();
    HWND window = nullptr;
    try {
        lab::Options options;
        bool selfTest = false, demoTour = false, underwaterView = false, inletView = false, dlssSettingsTest = false;
        std::string name = "lab";
        int argc = 0;
        std::unique_ptr<wchar_t *, CommandLineDeleter> arguments(
            CommandLineToArgvW(GetCommandLineW(), &argc));
        if (!arguments)
            throw std::runtime_error("Command line failed");
        auto argv = arguments.get();
        // The standalone portfolio application opens its flagship water demo.
        // Explicit command lines retain the existing fixture/scenario contracts.
        if (argc == 1) {
            options.fluidRoom = options.fluid = true;
        }
        bool normalLens = false, waveOptionsSet = false, waveControlsTest = false, wavePatchTest = false,
             waveFillTest = false, oceanControlsTest = false;
        // Preset defaults precede parsing so explicit overrides are order independent.
        for (int i = 1; i < argc; ++i)
            if (std::wstring_view(argv[i]) == L"--fluid-deep-pool") {
                options.fluidDeepPool = options.fluidRoom = options.fluid = options.fluidView = true;
                options.fluidParticles = lab::deepPool::particles;
                options.fluidCapacity = lab::deepPool::capacity;
                options.fluidDepth = lab::deepPool::depth;
                options.fluidCellSize = lab::deepPool::cellSize;
            }
        for (int i = 1; i < argc; ++i)
            if (std::wstring_view(argv[i]) == L"--water-lab=large") {
                options.largeWaterLab = options.fluidRoom = options.fluid = true;
                options.hamiltonian.enabled = true;
                options.fluidParticles = lab::largeWater::particles;
                options.fluidCapacity = lab::largeWater::capacity;
                options.fluidDepth = lab::largeWater::depth;
                options.fluidCellSize = lab::largeWater::cellSize;
            }
        for (int i = 1; i < argc; ++i)
            if (std::wstring_view(argv[i]) == L"--water-lab=ocean" ||
                std::wstring_view(argv[i]) == L"--water-lab=extra-large") {
                options.oceanLab = options.fluidRoom = options.fluid = options.boat = true;
                options.hamiltonian.enabled = true;
                options.hamiltonian.amplitude = .8f;
                options.hamiltonian.epsilon = 1;
                options.hamiltonian.resolution = 128;
                options.hamiltonian.windSpeed = 11;
                options.hamiltonian.extendOpticalSurface = true;
                options.fluidParticles = lab::ocean::particles;
                options.fluidCapacity = lab::ocean::capacity;
                options.fluidDepth = lab::ocean::depth;
                options.fluidCellSize = lab::ocean::cellSize;
                options.lasers = false;
            }
        // Suppress modal errors for bounded tests, even when another switch is invalid.
        for (int i = 1; i < argc; ++i)
            if (std::wstring_view(argv[i]).starts_with(L"--frames=") ||
                std::wstring_view(argv[i]) == L"--self-test")
                automation = true;
        const auto launchFolder = std::filesystem::current_path();
        std::filesystem::path folder = executableFolder();
        std::filesystem::current_path(folder);
        std::ofstream("NVMatrixEngine.log", std::ios::trunc)
            << "NVMatrixEngine | Native DXR Fluid Lab | Research preview\n";
        for (int i = 1; i < argc; ++i) {
            std::wstring wide = argv[i];
            if (wide.starts_with(L"--model=")) {
                if (wide.size() == 8) throw std::runtime_error("--model requires a .gltf, .glb or .obj path");
                options.models.push_back(std::filesystem::absolute(launchFolder / wide.substr(8)));
                continue;
            }
            std::string arg;
            for (wchar_t c : wide) {
                if (c > 127)
                    throw std::runtime_error("Lab switches must be ASCII");
                arg += char(c);
            }
            // Keep independent subsystem options outside MSVC's bounded
            // nesting depth for the older monolithic else-if parser.
            if (arg == "--scene=neon-night" || arg == "--neon-controls-test") {
                options.neonNight = true;
                options.neonControlsTest |= arg == "--neon-controls-test";
                normalLens = true;
                continue;
            }
            if (arg == "--dlss-settings-test") {
                dlssSettingsTest = true;
                continue;
            }
            if (arg == "--sampling-controls-test") {
                options.samplingControlsTest = true;
                continue;
            }
            if (arg == "--model-anyhit-reference") {
                options.modelAnyHitReference = true;
                continue;
            }
            if (arg == "--neon-reference") {
                options.sampling.mode = 0;
                options.sampling.roulette = false;
                continue;
            }
            if (arg.starts_with("--light-sampling=")) {
                auto mode = arg.substr(17);
                if (mode != "reference" && mode != "ris")
                    throw std::runtime_error("Light sampling must be reference or ris (NEE-AT was reverted)");
                options.sampling.mode = mode == "reference" ? 0u : 1u;
                continue;
            }
            if (arg.starts_with("--russian-roulette=")) {
                auto value = arg.substr(19);
                if (value != "on" && value != "off")
                    throw std::runtime_error("Russian roulette must be on or off");
                options.sampling.roulette = value == "on";
                continue;
            }
            if (arg.starts_with("--neon-path-samples=") || arg.starts_with("--neon-light-samples=") ||
                arg.starts_with("--neon-bounces=") || arg.starts_with("--neon-light-candidates=") ||
                arg.starts_with("--path-samples=") || arg.starts_with("--light-samples=") ||
                arg.starts_with("--path-bounces=") || arg.starts_with("--light-candidates=")) {
                auto count = number(arg.substr(arg.find('=') + 1), 1, 8);
                if (arg.find("path-samples=") != std::string::npos) options.sampling.paths = count;
                else if (arg.find("light-samples=") != std::string::npos) options.sampling.lights = count;
                else if (arg.find("light-candidates=") != std::string::npos) options.sampling.candidates = count;
                else options.sampling.bounces = count;
                continue;
            }
            if (arg == "--lambertian-reference") {
                options.lambertianReference = true;
                continue;
            }
            if (arg == "--water-visibility-reference") {
                options.waterVisibilityReference = true;
                continue;
            }
            if (arg == "--underwater-view") {
                underwaterView = true;
                continue;
            }
            if (arg == "--inlet-view") {
                inletView = true;
                continue;
            }
            if (arg == "--demo-tour") {
                demoTour = true;
                continue;
            }
            if (arg == "--normal-lens") {
                normalLens = true;
                continue;
            }
            if (arg.starts_with("--model-scale=") || arg.starts_with("--model-x=") ||
                arg.starts_with("--model-y=") || arg.starts_with("--model-z=")) {
                const auto text = arg.substr(arg.find('=') + 1);
                size_t consumed = 0;
                const float x = std::stof(text, &consumed);
                if (consumed != text.size() || !std::isfinite(x))
                    throw std::runtime_error("Model placement requires a finite number");
                if (arg.starts_with("--model-scale=")) {
                    if (x <= 0) throw std::runtime_error("--model-scale must be positive");
                    options.modelScale = x;
                } else if (arg[8] == 'x') options.modelPosition.x = x;
                else if (arg[8] == 'y') options.modelPosition.y = x;
                else options.modelPosition.z = x;
                continue;
            }
            if (arg == "--fluid-narrow-band") {
                options.fluidNarrowBand = options.fluidOwnedParticles = options.fluid = true;
                options.fluidBackend = "cuda";
                options.fluidCudaGraphs = true;
                continue;
            }
            auto value = [&](const char *prefix) { return arg.substr(strlen(prefix)); };
            auto physical = [&](const char *prefix) {
                const auto s = value(prefix);
                size_t consumed = 0;
                float x = std::stof(s, &consumed);
                if (consumed != s.size() || !std::isfinite(x) || x < 0)
                    throw std::runtime_error("Expected a finite, nonnegative SI material value");
                return x;
            };
            if (arg == "--water-lab=large" || arg == "--water-lab=ocean" || arg == "--water-lab=extra-large")
                continue;
            if (arg.starts_with("--time-of-day=")) {
                const auto mode = value("--time-of-day=");
                if (mode != "day" && mode != "night") throw std::runtime_error("Time of day must be day or night");
                options.oceanNight = mode == "night";
                continue;
            }
            if (arg.starts_with("--water-path=")) {
                const auto mode = value("--water-path=");
                if (mode != "baseline" && mode != "hamiltonian")
                    throw std::runtime_error("Water path must be baseline or hamiltonian");
                options.hamiltonian.enabled = mode == "hamiltonian";
                options.fluidRoom = options.fluid = true;
                continue;
            }
            if (arg == "--wave-patch-test") {
                wavePatchTest = true;
                continue;
            }
            if (arg == "--wave-controls-test") {
                waveControlsTest = true;
                continue;
            }
            if (arg == "--ocean-controls-test") {
                oceanControlsTest = true;
                continue;
            }
            if (arg == "--ocean-swim-test") {
                options.oceanSwimTest = true;
                continue;
            }
            if (arg == "--wave-fill-test") {
                waveFillTest = true;
                continue;
            }
            if (arg.starts_with("--wave-epsilon=")) {
                options.hamiltonian.epsilon = physical("--wave-epsilon=");
                waveOptionsSet = true;
                continue;
            }
            if (arg.starts_with("--wave-amplitude=")) {
                options.hamiltonian.amplitude = physical("--wave-amplitude=");
                waveOptionsSet = true;
                continue;
            }
            if (arg.starts_with("--wave-relaxation=")) {
                options.hamiltonian.relaxation = physical("--wave-relaxation=");
                waveOptionsSet = true;
                continue;
            }
            if (arg.starts_with("--wave-order=")) {
                const auto order = value("--wave-order=");
                if (order != "2" && order != "3")
                    throw std::runtime_error("Wave order must be 2 or 3");
                options.hamiltonian.order = order == "2" ? 2u : 3u;
                waveOptionsSet = true;
                continue;
            }
            // Keep backend selection out of the legacy else-if chain, which is
            // close to MSVC's syntactic nesting limit.
            if (arg == "--fluid-deep-pool")
                continue;
            if (arg.starts_with("--fluid-backend=")) {
                options.fluidBackend = value("--fluid-backend=");
                if (options.fluidBackend != "dx12" && options.fluidBackend != "cuda")
                    throw std::runtime_error("Fluid backend must be dx12 or cuda");
                options.fluid = true;
                continue;
            }
            if (arg == "--profile-latency") {
                options.profileLatency = true;
                continue;
            }
            if (arg == "--profile-fluid-bursts") {
                options.profileLatency = options.profileFluidBursts = options.fluidRoom = options.fluid =
                    true;
                continue;
            }
            if (arg.starts_with("--fluid-cuda-context=")) {
                const auto mode = value("--fluid-cuda-context=");
                if (mode != "primary" && mode != "cig")
                    throw std::runtime_error("CUDA context must be primary or cig");
                options.fluidCudaGraphicsContext = mode == "cig";
                continue;
            }
            if (arg.starts_with("--fluid-cuda-graphs=")) {
                const auto mode = value("--fluid-cuda-graphs=");
                if (mode != "on" && mode != "off")
                    throw std::runtime_error("CUDA graphs must be on or off");
                options.fluidCudaGraphs = mode == "on";
                continue;
            }
            if (arg.starts_with("--fluid-cuda-pressure=")) {
                options.fluidCudaPressure = value("--fluid-cuda-pressure=");
                if (options.fluidCudaPressure != "uniform" && options.fluidCudaPressure != "fine" &&
                    options.fluidCudaPressure != "mixed")
                    throw std::runtime_error("CUDA pressure must be uniform, fine or mixed");
                continue;
            }
            if (arg.starts_with("--fluid-cuda-pressure-loop=")) {
                const auto mode = value("--fluid-cuda-pressure-loop=");
                if (mode != "conditional" && mode != "unrolled")
                    throw std::runtime_error("CUDA pressure loop must be conditional or unrolled");
                options.fluidCudaConditionalPressure = mode == "conditional";
                continue;
            }
            if (arg.starts_with("--fluid-cuda-pressure-bricks=")) {
                options.fluidCudaPressureBricks = number(value("--fluid-cuda-pressure-bricks="), 1, 16384);
                continue;
            }
            if (arg.starts_with("--fluid-cuda-pressure-changes=")) {
                options.fluidCudaPressureChanges = number(value("--fluid-cuda-pressure-changes="), 1, 16384);
                continue;
            }
            if (arg.starts_with("--fluid-cuda-cg-iterations=")) {
                options.fluidCudaCgIterations = number(value("--fluid-cuda-cg-iterations="), 1, 32);
                continue;
            }
            if (arg.starts_with("--width="))
                options.width = number(value("--width="), 320, 7680);
            else if (arg.starts_with("--height="))
                options.height = number(value("--height="), 240, 4320);
            else if (arg.starts_with("--photons="))
                options.photons = number(value("--photons="), 1024, 1048576);
            else if (arg.starts_with("--frames="))
                options.frames = number(value("--frames="), 1, 36000);
            else if (arg.starts_with("--history="))
                options.history = number(value("--history="), 1, 256);
            else if (arg.starts_with("--quality=")) {
                const auto mode = value("--quality=");
                if (mode != "quality" && mode != "balanced" && mode != "performance")
                    throw std::runtime_error("Invalid DLSS quality mode");
                options.quality = mode == "quality" ? 0 : (mode == "balanced" ? 1 : 2);
            } else if (arg.starts_with("--frame-gen=")) {
                options.frameGeneration = value("--frame-gen=");
                if (options.frameGeneration != "off" && options.frameGeneration != "auto" &&
                    options.frameGeneration != "2" && options.frameGeneration != "3" &&
                    options.frameGeneration != "4")
                    throw std::runtime_error("Frame generation must be auto, off, 2, 3 or 4");
            } else if (arg == "--frame-gen-test")
                options.frameGenerationTest = true;
            else if (arg.starts_with("--ser="))
                options.ser = value("--ser=");
            else if (arg.starts_with("--atomics="))
                options.atomics = value("--atomics=");
            else if (arg.starts_with("--pose="))
                options.pose = value("--pose=");
            else if (arg.starts_with("--laser-nm="))
                options.laserNm = float(number(value("--laser-nm="), 380, 780));
            else if (arg == "--no-water")
                options.water = false;
            else if (arg == "--fluid" || arg == "--fluid-room")
                options.fluid = options.fluidRoom = true;
            else if (arg == "--fluid-pit") {
                options.fluid = true;
                options.fluidRoom = false;
            } else if (arg == "--fluid-emitter")
                options.fluidEmitter = true;
            else if (arg == "--fluid-room-test")
                options.fluidRoomTest = options.fluidRoom = options.fluidValidate = options.fluid = true;
            else if (arg == "--restir-pt")
                options.restirPt = true;
            else if (arg == "--adaptive-rays")
                options.adaptiveRays = options.opticalImportance = true;
            else if (arg == "--optical-importance")
                options.opticalImportance = true;
            else if (arg == "--optical-estimator-test")
                options.opticalEstimatorTest = options.opticalImportance = true;
            else if (arg == "--optical-validate")
                options.opticalValidate = options.opticalImportance = true;
            else if (arg == "--optical-controls-test")
                options.opticalControlsTest = options.adaptiveRays = options.opticalValidate =
                    options.opticalImportance = true;
            else if (arg == "--optical-reference")
                options.opticalUniform = options.opticalImportance = true;
            else if (arg == "--optical-freeze")
                options.opticalFreeze = options.opticalImportance = true;
            else if (arg == "--camera-retrace-primary")
                options.retracePrimary = true;
            else if (arg.starts_with("--optical-samples="))
                options.opticalSamples = uint32_t(number(value("--optical-samples="), 1, 8));
            else if (arg.starts_with("--optical-view=")) {
                const auto mode = value("--optical-view=");
                const std::array<std::string, 6> names{"importance", "variance", "temporal",
                                                       "caustics",   "rays",     "confidence"};
                const auto found = std::find(names.begin(), names.end(), mode);
                if (found == names.end())
                    throw std::runtime_error("Unknown optical importance view");
                options.opticalView = uint32_t(found - names.begin()) + 1;
                options.opticalImportance = true;
            } else if (arg == "--no-restir-pt")
                options.restirPt = false;
            else if (arg == "--fluid-full-brick-traversal")
                options.fluidFullBrickTraversal = true;
            else if (arg == "--fluid-adaptive")
                options.fluidAdaptive = options.fluid = true;
            else if (arg == "--fluid-mac")
                options.fluidMac = options.fluidAdaptive = options.fluid = true;
            else if (arg == "--fluid-mac-split-coarse") {
                options.fluidMacSplitCoarse = options.fluidMacMultigrid = options.fluidMac =
                    options.fluidAdaptive = options.fluid = true;
            } else if (arg.starts_with("--fluid-mac-solver=")) {
                const auto solver = value("--fluid-mac-solver=");
                if (solver != "multigrid" && solver != "relaxation")
                    throw std::runtime_error("Mixed MAC solver must be multigrid or relaxation");
                options.fluidMacMultigrid = solver == "multigrid";
                options.fluidMac = options.fluidAdaptive = options.fluid = true;
            } else if (arg == "--fluid-mac-validate")
                options.fluidMacValidate = options.fluidMac = options.fluidAdaptive = options.fluid = true;
            else if (arg == "--fluid-mac-view")
                options.fluidMacView = options.fluidMac = options.fluidAdaptive = options.fluid = true;
            else if (arg == "--fluid-mac-cycle-test")
                options.fluidMacCycle = options.fluidMac = options.fluidAdaptive = options.fluid = true;
            else if (arg == "--fluid-deterministic-bins")
                options.fluidDeterministicBins = options.fluid = true;
            else if (arg == "--fluid-sparse-work")
                options.fluidSparseWork = options.fluid = true;
            else if (arg == "--fluid-surface-lod")
                options.fluidSurfaceLod = options.fluidAdaptive = options.fluid = true;
            else if (arg.starts_with("--fluid-surface-lod-axes=")) {
                const auto axes = value("--fluid-surface-lod-axes=");
                uint32_t mask = 0;
                for (const char axis : axes) {
                    if (axis != 'x' && axis != 'y' && axis != 'z')
                        throw std::runtime_error("Surface LOD axes must be a nonempty subset of xyz");
                    const uint32_t bit = 1u << (axis - 'x');
                    if (mask & bit)
                        throw std::runtime_error("Surface LOD axes must not repeat");
                    mask |= bit;
                }
                if (!mask)
                    throw std::runtime_error("Surface LOD axes must be a nonempty subset of xyz");
                options.fluidSurfaceLodAxes = mask;
                options.fluidSurfaceLod = options.fluidAdaptive = options.fluid = true;
            } else if (arg == "--fluid-surface-lod-fine")
                options.fluidSurfaceLodFine = options.fluidSurfaceLod = options.fluidAdaptive =
                    options.fluid = true;
            else if (arg == "--fluid-surface-lod-validate")
                options.fluidSurfaceLodValidate = options.fluidSurfaceLod = options.fluidAdaptive =
                    options.fluid = true;
            else if (arg == "--fluid-surface-lod-view")
                options.fluidSurfaceLodView = options.fluidSurfaceLod = options.fluidAdaptive =
                    options.fluid = true;
            else if (arg == "--fluid-work-validate")
                options.fluidWorkValidate = options.fluidSparseWork = options.fluid = true;
            else if (arg == "--fluid-work-view")
                options.fluidWorkView = options.fluidView = options.fluidSparseWork = options.fluid = true;
            else if (arg == "--fluid-resample")
                options.fluidResample = options.fluidAdaptive = options.fluid = true;
            else if (arg == "--fluid-owned-particles")
                options.fluidOwnedParticles = options.fluid = true;
            else if (arg == "--fluid-interior")
                options.fluidInterior = options.fluidResample = options.fluidAdaptive = options.fluid = true;
            else if (arg == "--fluid-interior-validate")
                options.fluidInteriorValidate = options.fluidInterior = options.fluidResample =
                    options.fluidAdaptive = options.fluid = true;
            else if (arg == "--fluid-interior-cycle-test")
                options.fluidInteriorCycle = options.fluidInteriorValidate = options.fluidInterior =
                    options.fluidResample = options.fluidAdaptive = options.fluid = true;
            else if (arg == "--fluid-interior-wake-test")
                options.fluidInteriorWake = options.fluidInteriorValidate = options.fluidInterior =
                    options.fluidResample = options.fluidAdaptive = options.fluid = true;
            else if (arg == "--fluid-wake-test")
                options.fluidInteriorWake = options.fluid = true;
            else if (arg == "--fluid-resample-validate")
                options.fluidResampleValidate = options.fluidResample = options.fluidAdaptive =
                    options.fluid = true;
            else if (arg.starts_with("--fluid-depth="))
                options.fluidDepth = physical("--fluid-depth=");
            else if (arg.starts_with("--fluid-gravity="))
                options.fluidGravity = physical("--fluid-gravity=");
            else if (arg == "--fluid-complexity-validate")
                options.fluidComplexityValidate = options.fluidAdaptive = options.fluid = true;
            else if (arg == "--fluid-complexity-freeze")
                options.fluidComplexityFreeze = options.fluidAdaptive = options.fluid = true;
            else if (arg == "--fluid-complexity-controls-test")
                options.fluidComplexityControlsTest = options.fluidComplexityValidate =
                    options.fluidAdaptive = options.fluid = true;
            else if (arg.starts_with("--fluid-complexity-view=")) {
                options.fluidComplexityView = number(value("--fluid-complexity-view="), 0, 10);
                options.fluidAdaptive = options.fluid = true;
            } else if (arg.starts_with("--pt-spatial="))
                options.ptSpatial = number(value("--pt-spatial="), 0, 2);
            else if (arg.starts_with("--pt-history="))
                options.ptHistory = number(value("--pt-history="), 1, 16);
            else if (arg == "--no-whitewater")
                options.whitewater = false;
            else if (arg.starts_with("--fluid-capacity="))
                options.fluidCapacity = number(value("--fluid-capacity="), 1, 1000000);
            else if (arg == "--fluid-solver-only")
                options.fluidSolverOnly = options.fluid = true;
            else if (arg == "--fluid-view")
                options.fluidView = options.fluid = true;
            else if (arg == "--fluid-isotropic")
                options.fluidIsotropic = true;
            else if (arg == "--fluid-collider-test")
                options.fluidColliderTest = options.fluidValidate = options.fluid = true;
            else if (arg == "--fluid-surface-controls-test")
                options.fluidSurfaceControlsTest = options.fluidValidate = options.fluid = true;
            else if (arg.starts_with("--fluid-surface-fixture="))
                options.fluidSurfaceFixture = number(value("--fluid-surface-fixture="), 1, 4);
            else if (arg == "--fluid-validate")
                options.fluidValidate = options.fluid = true;
            else if (arg == "--fluid-controls-test")
                options.fluidControlTest = options.fluidValidate = options.fluid = true;
            else if (arg.starts_with("--fluid-transfer-test="))
                options.fluidTransferTest = number(value("--fluid-transfer-test="), 1, 4);
            else if (arg == "--fluid-ballistic")
                options.fluidBallistic = true;
            else if (arg == "--fluid-flip")
                options.fluidFlip = true;
            else if (arg.starts_with("--fluid-pressure-iterations="))
                options.fluidPressureIterations = number(value("--fluid-pressure-iterations="), 1, 1000);
            else if (arg.starts_with("--fluid-density-iterations="))
                options.fluidDensityIterations = number(value("--fluid-density-iterations="), 0, 1000);
            else if (arg == "--fluid-density-scalar")
                options.fluidDensityScalar = true;
            else if (arg.starts_with("--fluid-pressure=")) {
                options.fluidPressure = value("--fluid-pressure=");
                if (options.fluidPressure != "uniform" && options.fluidPressure != "active" &&
                    options.fluidPressure != "multigrid")
                    throw std::runtime_error("Fluid pressure must be uniform, active, or multigrid");
            } else if (arg.starts_with("--fluid-pressure-cycles="))
                options.fluidPressureCycles = number(value("--fluid-pressure-cycles="), 1, 16);
            else if (arg == "--fluid-pressure-validate")
                options.fluidPressureValidate = true;
            else if (arg == "--fluid-bulk")
                options.fluidBulk = true;
            else if (arg == "--fluid-bulk-projected" || arg == "--fluid-bulk-capacity" ||
                     arg == "--fluid-bulk-bounded") {
                options.fluidBulkBounded |= arg == "--fluid-bulk-bounded";
                options.fluidBulkCapacity |= arg == "--fluid-bulk-capacity" || options.fluidBulkBounded;
                options.fluidBulkProjected = options.fluidBulk = options.fluidCutPressure =
                    options.fluidCutCells = options.fluidMac = options.fluidMacMultigrid =
                        options.fluidAdaptive = options.fluid = true;
            } else if (arg == "--fluid-bulk-pressure") {
                options.fluidBulkPressure = options.fluidBulkBounded = options.fluidBulkCapacity =
                    options.fluidBulkProjected = options.fluidBulk = options.fluidCutPressure =
                        options.fluidCutTimeCentered = options.fluidCutCells = options.fluidMac =
                            options.fluidMacMultigrid = options.fluidAdaptive = options.fluid = true;
            } else if (arg == "--fluid-bulk-implicit" || arg == "--fluid-bulk-air" ||
                       arg == "--fluid-bulk-coupled") {
                options.fluidBulkCoupled |= arg == "--fluid-bulk-coupled";
                options.fluidBulkAirExtension |= arg == "--fluid-bulk-air";
                options.fluidBulkAirExtension |= options.fluidBulkCoupled;
                options.fluidBulkImplicit = options.fluidBulkPressure = options.fluidBulkCapacity =
                    options.fluidBulkProjected = options.fluidBulk = options.fluidCutPressure =
                        options.fluidCutTimeCentered = options.fluidCutCells = options.fluidMac =
                            options.fluidMacMultigrid = options.fluidAdaptive = options.fluid = true;
            } else if (arg.starts_with("--fluid-capacity-cycles="))
                options.fluidCapacityCycles = number(value("--fluid-capacity-cycles="), 1, 4);
            else if (arg == "--fluid-cut-cells")
                options.fluidCutCells = options.fluid = true;
            else if (arg == "--fluid-cut-kernel-full")
                options.fluidCutKernelCache = false;
            else if (arg == "--fluid-cut-time-centered") {
                options.fluidCutTimeCentered = true;
                options.fluidCutPressure = options.fluidCutCells = options.fluidMac =
                    options.fluidMacMultigrid = options.fluidAdaptive = options.fluid = true;
            } else if (arg == "--fluid-cut-pressure")
                options.fluidCutPressure = options.fluidCutCells = options.fluidMac =
                    options.fluidMacMultigrid = options.fluidAdaptive = options.fluid = true;
            else if (arg == "--fluid-cut-validate")
                options.fluidCutValidate = options.fluidCutCells = options.fluid = true;
            else if (arg == "--fluid-cut-view")
                options.fluidCutView = options.fluidCutCells = options.fluid = true;
            else if (arg.starts_with("--fluid-cut-fixture=")) {
                options.fluidCutValidate = options.fluidCutCells = options.fluid = true;
                options.fluidCutFixture = number(value("--fluid-cut-fixture="), 1, 4);
            } else if (arg == "--fluid-bulk-validate")
                options.fluidBulk = options.fluidBulkValidate = true;
            else if (arg.starts_with("--fluid-bulk-fixture=")) {
                options.fluidBulk = options.fluidBulkValidate = true;
                options.fluidBulkFixture = number(value("--fluid-bulk-fixture="), 1, 3);
            } else if (arg.starts_with("--fluid-bulk-view=")) {
                options.fluidBulk = true;
                options.fluidBulkView = number(value("--fluid-bulk-view="), 0, 2);
            } else if (arg.starts_with("--fluid-viscosity="))
                options.fluidViscosity = physical("--fluid-viscosity=");
            else if (arg.starts_with("--fluid-surface-tension="))
                options.fluidSurfaceTension = physical("--fluid-surface-tension=");
            else if (arg.starts_with("--fluid-material-test="))
                options.fluidMaterialTest = number(value("--fluid-material-test="), 1, 2);
            else if (arg == "--fisheye")
                options.fisheye = true;
            else if (arg == "--boat")
                options.boat = true;
            else if (arg == "--experience-test") {
                options.experienceTest = options.boat = options.fisheye = options.fluidRoom = options.fluid =
                    true;
                options.water = false;
            } else if (arg.starts_with("--fluid-cell-size="))
                options.fluidCellSize =
                    std::clamp(physical("--fluid-cell-size="),
                               options.oceanLab ? .8f : options.fluidDeepPool ? .5f : (options.largeWaterLab ? .20f : .10f),
                               options.oceanLab ? 1.2f : options.fluidDeepPool ? 1.f : (options.largeWaterLab ? .32f : .24f));
            else if (arg.starts_with("--fluid-simulation-hz="))
                options.fluidSimulationHz =
                    std::clamp(std::stof(value("--fluid-simulation-hz=")), 60.f, 180.f);
            else if (arg.starts_with("--fluid-particles="))
                options.fluidParticles = number(value("--fluid-particles="), 0, 1000000);
            else if (arg == "--gpu-validation")
                options.gpuValidation = options.debug = true;
            else if (arg == "--flat-water")
                options.flatWater = true;
            else if (arg == "--no-lasers")
                options.lasers = false;
            else if (arg == "--no-haze")
                options.haze = false;
            else if (arg.starts_with("--name=")) {
                name = value("--name=");
                if (name.empty() || name.find_first_not_of(
                                        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
                                        std::string::npos)
                    throw std::runtime_error("Unsafe report name");
            } else if (arg == "--debug")
                options.debug = true;
            else if (arg == "--dred")
                options.dred = true;
            else if (arg == "--animate")
                options.animate = true;
            else if (arg == "--capture")
                options.capture = true;
            else if (arg == "--fixture")
                options.fixture = true;
            else if (arg == "--gameplay-test")
                options.gameplayTest = true;
            else if (arg == "--temporal-test")
                options.temporalTest = true;
            else if (arg == "--fluid-temporal-test")
                options.fluidTemporalTest = options.fluid = options.fluidRoom = true;
            else if (arg == "--orbit-test")
                options.orbitTest = true;
            else if (arg == "--rolling-test")
                options.rollingTest = true;
            else if (arg == "--self-test")
                selfTest = true;
            else
                throw std::runtime_error("Unknown lab argument: " + arg);
        }
        if (options.samplingControlsTest && (options.frames != 96 || options.fixture))
            throw std::runtime_error("Sampling controls test requires --frames=96 and a gameplay scene");
        if (options.neonNight) {
            if (options.fixture || options.fluid || options.animate || options.temporalTest || options.orbitTest || options.gameplayTest)
                throw std::runtime_error("Neon Night is a playable alley; remove optical/fluid/legacy gameplay fixtures");
            if (options.neonControlsTest && options.frames != 180)
                throw std::runtime_error("Neon controls test requires --frames=180");
            options.water = options.lasers = options.haze = false;
            options.models.insert(options.models.begin(), folder / "assets/neon-night/neon-night.gltf");
        }
        if (selfTest) {
            lab::runPlayTests();
            runAudioTests(folder / "assets/audio");
            logLine("PASS: lab Bullet mechanics, tuning/cancel, grab/throw, receiver charge and exit");
            return 0;
        }
        if (dlssSettingsTest && (options.frames != 200 || options.fixture || options.quality != 0))
            throw std::runtime_error("DLSS settings test requires --frames=200, Quality and a gameplay scene");
        if (options.gameplayTest && (!options.frames || options.frames < 360 || options.fixture))
            throw std::runtime_error("Gameplay test needs at least 360 frames and the playable scene");
        if (options.temporalTest && (options.frames != 256 || !options.fixture || options.animate))
            throw std::runtime_error("Temporal test needs --fixture --frames=256 without --animate");
        if (options.fluidTemporalTest &&
            (options.frames != 320 || options.fixture || options.gameplayTest || options.temporalTest ||
             options.orbitTest || options.rollingTest || options.fluidView || options.pose != "initial"))
            throw std::runtime_error(
                "Fluid temporal test needs 320 initial playable frames without other camera tests");
        if (options.orbitTest && (!options.frames || options.fixture || options.gameplayTest))
            throw std::runtime_error("Orbit test needs a bounded playable run without --gameplay-test");
        if (options.rollingTest && (options.frames != 300 || options.fixture || options.gameplayTest ||
                                    options.orbitTest || options.pose != "initial"))
            throw std::runtime_error("Rolling test needs 300 initial playable frames without other tests");
        if (options.pose != "initial" && options.pose != "solved" && options.pose != "blocked" &&
            options.pose != "tuning")
            throw std::runtime_error("Invalid playtest pose");
        if (options.pose != "initial" && (!options.frames || options.fixture))
            throw std::runtime_error("Playtest poses require a bounded playable run");
        if (options.atomics != "auto" && options.atomics != "float" && options.atomics != "fixed")
            throw std::runtime_error("Invalid atomics mode");
        if (options.capture && !options.frames)
            throw std::runtime_error("Capture requires a bounded --frames run");
        automation = options.frames != 0;
        if (oceanControlsTest && (!options.oceanLab || options.oceanNight || options.frames != 120))
            throw std::runtime_error("Ocean controls test requires daytime Ocean Lab and --frames=120");
        if (options.oceanSwimTest && (!options.oceanLab || options.frames != 600))
            throw std::runtime_error("Ocean swimming test requires Ocean Lab and --frames=600");
        if (options.oceanLab && (options.largeWaterLab || options.fluidDeepPool || options.fixture ||
                                !options.fluidRoom || options.fluidSolverOnly || options.fluidRoomTest ||
                                options.fluidTemporalTest || options.fluidCellSize < .8f || options.fluidBackend == "cuda"))
            throw std::runtime_error("Ocean Lab requires its own rendered room and cells >= 0.8 m");
        if (wavePatchTest && (!options.largeWaterLab || !options.hamiltonian.enabled || options.frames != 240))
            throw std::runtime_error("Wave patch test requires large Hamiltonian Water Lab and --frames=240");
        if (waveControlsTest && (!options.hamiltonian.enabled || options.frames != 12))
            throw std::runtime_error("Wave input test requires --water-path=hamiltonian --frames=12");
        if (waveFillTest && (!options.hamiltonian.enabled || options.frames < 240 || waveControlsTest || wavePatchTest))
            throw std::runtime_error("Wave fill test requires Hamiltonian water, at least 240 frames and no other wave fixture");
        if (waveOptionsSet && !options.hamiltonian.enabled)
            throw std::runtime_error("Wave controls require --water-path=hamiltonian");
        if (options.largeWaterLab && (options.fluidDeepPool || !options.fluidRoom || options.fixture ||
                                      options.fluidSolverOnly || options.fluidRoomTest ||
                                      options.fluidTemporalTest || options.fluidCellSize < .20f))
            throw std::runtime_error("Large Water Lab requires its own rendered room and cells >= 0.20 m");
        if (options.hamiltonian.enabled) {
            options.hamiltonian.validate(options.fluidDepth, options.fluidGravity);
            if (!options.fluidParticles)
                throw std::runtime_error("Hamiltonian water needs a positive full-room particle count");
            if (options.fluidDeepPool || options.fixture || options.fluidSurfaceLod ||
                options.fluidSurfaceFixture || options.fluidSurfaceControlsTest ||
                options.fluidRoomTest || options.fluidValidate || options.fluidControlTest ||
                options.fluidTemporalTest || options.fluidSolverOnly)
                throw std::runtime_error("Hamiltonian water uses the rendered room with its own open "
                                         "boundary reservoir; closed-volume fixtures and surface "
                                         "LOD are not supported");
        }
        if (underwaterView) {
            if (!options.fluidRoom || !options.boat)
                throw std::runtime_error("Underwater view requires --fluid-room and --boat");
            options.fluidView = false;
        }
        if (inletView && (!options.fluidRoom || options.fixture || options.fluidSolverOnly || underwaterView))
            throw std::runtime_error("Inlet view requires a rendered Water Lab room");
        if (demoTour && (!automation || !options.fluidRoom || !options.boat || options.fluidDeepPool ||
                         options.fixture || options.rollingTest || options.experienceTest))
            throw std::runtime_error("Demo tour requires a bounded fluid-room run with --boat");
        if (options.fluidDeepPool && (!options.fluidRoom || options.fixture || options.fluidSolverOnly ||
                                      options.fluidRoomTest || options.fluidTemporalTest))
            throw std::runtime_error("Deep pool is a separate rendered room, not a legacy room fixture");
        if (options.fluidBackend != "cuda" &&
            (options.fluidCudaGraphs || options.fluidCudaPressure != "uniform" ||
             options.fluidCudaPressureBricks != 512 || options.fluidCudaPressureChanges != 64 ||
             options.fluidCudaCgIterations != 32 || !options.fluidCudaConditionalPressure))
            throw std::runtime_error("CUDA settings require --fluid-backend=cuda");
        options.fisheye = !normalLens && (options.fisheye || (!automation && !options.fixture && argc > 1));
        options.boat = options.boat || (!automation && options.fluidRoom);
        if (options.boat && (!options.fluidRoom || !options.fluid || options.fluidSolverOnly))
            throw std::runtime_error("Boat requires the rendered --fluid-room scenario");
        if (options.experienceTest && options.frames != 240)
            throw std::runtime_error("Experience validation needs --frames=240");
        if (options.frameGenerationTest &&
            (options.frames != 180 || options.frameGeneration != "2" || options.fixture))
            throw std::runtime_error(
                "Frame generation input test needs --frames=180 --frame-gen=2 in the playable lab");
        if (options.fluidValidate && !options.frames)
            throw std::runtime_error("Fluid validation requires a bounded --frames run");
        if (options.fluidMaterialTest) {
            if (!options.fluidValidate || options.fluidTransferTest || options.fluidBallistic ||
                options.fluidSurfaceFixture || options.fluidControlTest || options.fluidSurfaceControlsTest)
                throw std::runtime_error(
                    "Material fixtures require bounded fluid validation without other fixtures");
            options.fluidSolverOnly = true;
        }
        if (options.fluidSurfaceFixture && (!options.fluidValidate || options.fluidSolverOnly))
            throw std::runtime_error("Surface fixtures require bounded --fluid-validate rendering");
        if (options.fluidSurfaceControlsTest &&
            (options.frames != 12 || options.fluidSolverOnly || options.fluidControlTest ||
             options.fluidTransferTest || options.fluidBallistic || options.fluidSurfaceFixture ||
             options.fluidView))
            throw std::runtime_error(
                "Surface input test requires 12 rendered fluid frames and the initial gameplay camera");
        if (options.fluidTransferTest && (!options.frames || !options.fluidValidate))
            throw std::runtime_error("Transfer fixture requires --fluid-validate --frames");
        if (options.fluidBallistic && !options.fluidValidate)
            throw std::runtime_error("Ballistic mode is a fluid validation fixture only");
        if (options.fluidControlTest && (options.frames != 12 || options.fluidTransferTest ||
                                         options.fluidBallistic || options.fluidFlip))
            throw std::runtime_error("Fluid input test requires 12 frames and default APIC mode");
        if (options.fluidFlip && (!options.fluid || options.fluidTransferTest))
            throw std::runtime_error("FLIP requires --fluid without APIC transfer fixtures");
        if (options.fluid && options.fixture)
            throw std::runtime_error("Fluid needs the playable test chamber");
        if (options.fluidOwnedParticles && (options.fluidResample || options.fluidInterior ||
                                            options.fluidBulk || options.fluidSurfaceFixture))
            throw std::runtime_error(
                "Owned particles require actual simulation without legacy resampling/interior/bulk replicas");
        if (options.fluid)
            options.water = false; // Fluid procedural geometry replaces the static snapshot.
        if (options.fluid && options.opticalImportance && !options.fluidSolverOnly)
            options.fluidAdaptive = true;
        if (options.fluidBallistic || options.fluidTransferTest || options.fluidControlTest)
            options.fluidSolverOnly = true;
        if (options.fluidAdaptive && options.fluidSolverOnly)
            throw std::runtime_error("Adaptive importance requires the reconstructed fluid surface");
        if (options.fluidComplexityValidate && !options.frames)
            throw std::runtime_error("Complexity validation requires bounded --frames");
        if (options.fluidMacValidate && !options.frames)
            throw std::runtime_error("Adaptive MAC validation requires bounded --frames");
        if (options.fluidWorkValidate && !options.frames)
            throw std::runtime_error("Fluid work validation requires bounded --frames");
        if (options.fluidSurfaceLodValidate && !options.frames)
            throw std::runtime_error("Surface LOD validation requires bounded --frames");
        if (options.opticalValidate && !options.frames)
            throw std::runtime_error("Optical validation requires bounded --frames");
        if (options.opticalControlsTest && options.frames != 48)
            throw std::runtime_error("Optical controls test requires 48 frames");
        if (options.opticalEstimatorTest && options.frames != 256)
            throw std::runtime_error("Optical estimator test requires 256 frames");
        if (options.fluidSparseWork && options.fluidDensityScalar)
            throw std::runtime_error("Compact density work uses paired sweeps; omit --fluid-density-scalar");
        if (options.fluidMacCycle && options.frames != 120)
            throw std::runtime_error("Adaptive MAC input cycle requires 120 frames");
        if (options.fluidMac && options.fluidPressure != "uniform")
            throw std::runtime_error(
                "Adaptive MAC uses a mixed-grid operator; fine-grid pressure mode must be uniform");
        if (options.fluidResampleValidate && !options.frames)
            throw std::runtime_error("Particle resampling validation requires bounded --frames");
        if (options.fluidInteriorValidate && !options.frames)
            throw std::runtime_error("Interior validation requires bounded --frames");
        if (options.fluidInteriorCycle && options.frames != 120)
            throw std::runtime_error("Interior force-fine cycle requires 120 frames");
        if (options.fluidInteriorWake && (options.frames != 180 || options.fluidRoom))
            throw std::runtime_error("Interior moving-solid fixture requires 180 pit frames");
        if (options.fluidResample && options.fluidSurfaceFixture)
            throw std::runtime_error("Particle resampling requires the actual reconstructed surface");
        if (options.fluidPressureValidate &&
            (!options.frames || !options.fluid || options.fluidPressure == "uniform"))
            throw std::runtime_error(
                "Pressure operator validation requires a bounded active/multigrid fluid run");
        if (options.fluidBulk && (!options.fluid || options.fluidSurfaceFixture))
            throw std::runtime_error(
                "Bulk inventory requires simulated fluid, not an optical surface fixture");
        if (options.fluidBulkValidate && !options.frames)
            throw std::runtime_error("Bulk validation/fixtures require bounded --frames");
        if (options.fluidCutValidate && !options.frames)
            throw std::runtime_error("Cut-cell validation/fixtures require bounded --frames");
        if (options.fluidCutPressure && options.fluidCutFixture)
            throw std::runtime_error(
                "Cut pressure cannot use geometry-only fixtures instead of actual colliders");
        if (options.fluidCutCells && (options.fluidBallistic || options.fluidSurfaceFixture ||
                                      options.fluidTransferTest || options.fluidMaterialTest))
            throw std::runtime_error("Cut cells require the normal simulated APIC/FLIP path");
        if (options.fluidComplexityControlsTest &&
            (options.frames != 16 || options.fluidSurfaceControlsTest || options.fluidRoomTest ||
             options.fluidSurfaceFixture))
            throw std::runtime_error(
                "Complexity controls test requires 16 frames without other input fixtures");
        if (options.fluidRoom && (options.fluidSolverOnly || options.fluidSurfaceFixture))
            throw std::runtime_error(
                "Room liquid is not a pit/solver fixture; use --fluid-pit for legacy tests");
        if (options.fluidEmitter && !options.fluidRoom)
            throw std::runtime_error("The wall inlet requires --fluid-room");
        if (options.profileFluidBursts && (options.frames != 300 || options.fixture || options.gameplayTest ||
                                           options.orbitTest || options.rollingTest || options.fluidValidate))
            throw std::runtime_error("Fluid burst profiling needs 300 unvalidated room frames");
        if (options.fluidRoomTest && options.frames != 180)
            throw std::runtime_error("Room interaction test requires 180 frames");
        viewWidth = options.width;
        viewHeight = options.height;
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        WNDCLASSW wc{};
        wc.hInstance = instance;
        wc.lpfnWndProc = windowProc;
        wc.lpszClassName = L"NVMatrixEngine";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassW(&wc))
            throw std::runtime_error("Register lab window failed");
        RECT rect{0, 0, LONG(options.width), LONG(options.height)};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        window = CreateWindowW(wc.lpszClassName, options.neonNight ? L"NOCTURNE | NVMatrixEngine" : L"NVMatrixEngine • Fluid Lab",
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                               rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
        if (!window)
            throw std::runtime_error("Create lab window failed");
        if (!automation) {
            RAWINPUTDEVICE mouse{0x01, 0x02, 0, window}; // Foreground only; keep legacy UI/tuning events.
            rawMouse = RegisterRawInputDevices(&mouse, 1, sizeof(mouse)) != FALSE;
            logLine(rawMouse ? "Orbit: raw mouse input" : "Orbit: legacy mouse fallback");
        }
        lab::StartupScreen startup(window, options.neonNight ? L"Neon Night" : L"NVMatrixEngine");
        activeStartup = &startup;
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
        {
            lab::Renderer renderer(window, folder, options, &startup);
            // This explicit UI fixture exercises Float -> Sink -> Float. The
            // actual Water Lab default is solid glass / Sink.
            if (options.experienceTest)
                renderer.experience.ballFloats = true;
            if (underwaterView)
                renderer.experience.firstPerson = true;
            std::unique_ptr<Game> game;
            std::unique_ptr<lab::Hud> hud;
            if (!options.fixture) {
                startup.run(L"Building player collisions", [&] {
                    game = std::make_unique<Game>(std::vector<Level>{renderer.playLevel()});
                });
                if (options.oceanLab) {
                    game->azimuth = 1.25f;
                    game->elevation = .30f;
                    game->distance = 14.f;
                }
                if (underwaterView)
                    game->elevation = -.10f;
                if (inletView) {
                    const auto inlet = renderer.fluid->emitter.position;
                    game->place(0, {inlet.x + 2.f, options.fluidDepth + .8f, inlet.z + 1.8f});
                    game->azimuth = 1.05f;
                    game->elevation = .28f;
                    game->distance = 5.5f;
                }
                startup.phase(L"Preparing controls");
                hud = std::make_unique<lab::Hud>(renderer.uiDevice(), window, folder, *game,
                                                 renderer.experience, automation);
                hud->rendererStatus = options.restirPt
                                          ? "RTXDI ReSTIR PT: opaque diffuse GI · photon caustics · DLSS-RR"
                                          : "Baseline diffuse GI · photon caustics · DLSS-RR";
                if (options.neonNight)
                    hud->rendererStatus = "Imported PBR materials · emissive area lights · DLSS Ray Reconstruction";
                hud->hamiltonianWater = options.hamiltonian.enabled;
                hud->largeWaterLab = options.largeWaterLab;
                if (options.hamiltonian.enabled)
                    hud->rendererStatus = "Hamiltonian HOS-" + std::to_string(options.hamiltonian.order) +
                                          " waves + local 3D flow · " + hud->rendererStatus;
                if (options.fluidColliderTest) {
                    game->place(0, {2.7f, .72f, -3.7f});
                    game->place(1, {3.65f, 1.2f, -3.1f}, XM_PI / 4);
                    game->place(2, {4.7f, .4f, -2.5f});
                }
                if (options.pose != "initial") {
                    game->place(1, {0, 1.60f, 0}, XM_PI / 6);
                    game->dock(1);
                    if (options.pose == "blocked")
                        game->place(4, {0, 2, -2});
                    if (options.pose == "tuning")
                        game->beginTuning();
                }
            }
            startup.phase(L"Opening scene");
            startup.finish();
            activeStartup = nullptr;
            activeRenderer = &renderer;
            activeGame = game.get();
            activeHud = hud.get();
            SetWindowTextW(window, options.neonNight ? L"NOCTURNE | NVMatrixEngine" : L"NVMatrixEngine | Fluid Lab");
            logLine("Startup: ready in " + std::to_string(std::chrono::duration<double>(
                std::chrono::steady_clock::now() - applicationStart).count()) + " s");
            auto previousTime = std::chrono::steady_clock::now();
            XMFLOAT3 testStart{};
            XMFLOAT3 boatTestStart{};
            bool boatMoved = false, boatWet = false;
            float swimStartHeight = 0;
            uint32_t movementRrResets = 0;
            float orbitStart = 0;
            auto require = [](bool ok, const char *why) {
                if (!ok)
                    throw std::runtime_error(why);
            };
            auto send = [&](UINT msg, WPARAM w, LPARAM l = 0) {
                injected = true;
                SendMessageW(window, msg, w, l);
                injected = false;
            };
            auto key = [&](WPARAM w) {
                send(WM_KEYDOWN, w);
                send(WM_KEYUP, w);
            };
            // Preserve user resizes received while background setup was running.
            if (pendingWidth == options.width && pendingHeight == options.height)
                pendingWidth = pendingHeight = 0;
            auto pumpMessages = [&] {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT)
                        running = false;
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            };
            auto applyResize = [&] {
                if (renderer.experience.rebuildWater)
                    renderer.applyWaterSettings();
                if (game) {
                    game->firstPerson = renderer.experience.firstPerson;
                    game->setBallFloating(renderer.experience.ballFloats);
                }
                if (hud && hud->requestedFrameMultiplier >= 1) {
                    renderer.setFrameGeneration(uint32_t(hud->requestedFrameMultiplier));
                    hud->requestedFrameMultiplier = -1;
                }
                if (hud && hud->requestedDlssQuality >= 0) {
                    renderer.setDlssQuality(hud->requestedDlssQuality);
                    hud->requestedDlssQuality = -1;
                }
                if (pendingWidth && pendingHeight) {
                    renderer.resize(pendingWidth, pendingHeight);
                    pendingWidth = pendingHeight = 0;
                }
            };
            while (running && (!options.frames || renderer.frame < options.frames)) {
                const auto wholeFrameStart = std::chrono::steady_clock::now();
                pumpMessages();
                if (!running)
                    break;
                applyResize();
                if (!renderer.waitForFrame())
                    continue;
                if (dlssSettingsTest) {
                    const auto f = renderer.frame;
                    if (f == 8 || f == 40 || f == 56 || f == 88 || f == 104 || f == 144)
                        key(VK_ESCAPE);
                    if (f == 16 || f == 64 || f == 112 || f == 136) {
                        const int quality = f == 16 ? 1 : f == 64 ? 2 : 0;
                        const char *ids[] = {"dlss-quality", "dlss-balanced", "dlss-performance"};
                        const auto oldWidth = renderer.streamlineState().renderWidth;
                        const auto oldResets = renderer.rrHistoryResets;
                        const auto *water = renderer.fluid.get();
                        const auto particles = water ? water->activeParticles : 0;
                        const auto seconds = water ? water->advancedSeconds : 0;
                        hud->testClick(ids[quality]);
                        require(hud->requestedDlssQuality == quality, "DLSS preset button did not request its mode");
                        applyResize();
                        const auto &sl = renderer.streamlineState();
                        require(renderer.dlssQuality() == quality, "DLSS preset was not applied");
                        require(f == 136 ? sl.renderWidth == oldWidth
                                        : quality == 0 ? sl.renderWidth > oldWidth : sl.renderWidth < oldWidth,
                                "DLSS preset did not change the actual rendering resolution");
                        require(sl.width == options.width && sl.height == options.height,
                                "DLSS preset changed the display resolution");
                        require(renderer.fluid.get() == water &&
                                    (!water || (water->activeParticles == particles && water->advancedSeconds == seconds)),
                                "DLSS preset reset the water simulation");
                        require(renderer.rrHistoryResets == oldResets, "DLSS preset rendered outside the frame boundary");
                        logLine("PASS DLSS menu preset " + std::to_string(quality) + " at frame " +
                                std::to_string(f) + "; water preserved");
                    }
                    if (f == 160 || f == 184)
                        hud->testClick("lens");
                    if (f == 166 || f == 174 || f == 188)
                        hud->testValue("lens-fov", f == 166 ? 120.f : f == 174 ? 160.f : 90.f);
                    if (f == 180 || f == 192)
                        send(WM_SIZE, SIZE_RESTORED,
                             MAKELPARAM(options.width + (f == 180 ? 1 : 0),
                                        options.height + (f == 180 ? 1 : 0)));
                    applyResize();
                }
                if (options.samplingControlsTest) {
                    auto &s = renderer.experience.sampling;
                    switch (renderer.frame) {
                    case 10: key(VK_ESCAPE); break;
                    case 12:
                        require(game->paused && hud->testVisible("pause"), "Sampling menu did not open");
                        hud->testClick("light-sampling"); break;
                    case 14:
                        require(s.mode == 0, "Reference sampling menu option failed");
                        hud->testValue("path-samples", 2); hud->testValue("light-samples", 3);
                        hud->testValue("light-candidates", 5); hud->testValue("path-bounces", 5);
                        hud->testClick("russian-roulette"); break;
                    case 16:
                        require(s.paths == 2 && s.lights == 3 && s.candidates == 5 && s.bounces == 5 && !s.roulette,
                                "Engine-wide sampling controls did not apply"); break;
                    case 20: hud->testClick("light-sampling"); break;
                    case 24: require(s.mode == 1, "RIS sampling menu option failed"); break;
                    case 28: hud->testClick("light-sampling"); break;
                    case 30: require(s.mode == 0, "Reference re-selection failed"); break;
                    case 32: hud->testClick("light-sampling"); break;
                    case 36:
                        hud->testValue("path-samples", 1); hud->testValue("light-samples", 2);
                        hud->testValue("light-candidates", 4); hud->testValue("path-bounces", 4);
                        hud->testClick("russian-roulette"); break;
                    case 44: case 48: hud->testClick("flashlight"); break;
                    case 52: hud->testClick("dlss-balanced"); break;
                    case 60: hud->testClick("dlss-quality"); break;
                    case 64: key(VK_ESCAPE); break;
                    case 80: key('R'); break;
                    case 95:
                        require(s == lab::SamplingSettings{} && !game->paused, "Sampling test did not restore defaults");
                        logLine("PASS: engine-wide reference/RIS, sample budgets, roulette, flashlight, resize and restart");
                        break;
                    }
                }
                if (options.neonControlsTest) {
                    auto &g = *game;
                    switch (renderer.frame) {
                    case 10:
                        require(g.grounded(), "Neon player did not settle on imported pavement");
                        testStart = g.playerPosition();
                        send(WM_KEYDOWN, 'W');
                        break;
                    case 42: key(VK_SPACE); break;
                    case 50: send(WM_KEYUP, 'W'); break;
                    case 56:
                        require(g.playerPosition().z > testStart.z + .8f && g.jumps == 1,
                                "Neon keyboard movement/jump failed");
                        key(VK_TAB);
                        break;
                    case 60:
                        require(g.firstPerson, "Neon Tab did not switch to first person");
                        key(VK_TAB);
                        break;
                    case 90:
                        key(VK_ESCAPE);
                        require(g.paused, "Neon Esc did not pause");
                        testStart = g.playerPosition();
                        break;
                    case 92:
                        require(hud->testVisible("pause"), "Neon settings overlay is hidden");
                        require(!hud->testVisible("receiver") && !hud->testVisible("water-settings"),
                                "Neon HUD retained receiver/liquid controls");
                        hud->testClick("dlss-balanced");
                        break;
                    case 94:
                        require(renderer.dlssQuality() == 1 && renderer.streamlineState().renderWidth < options.width,
                                "Neon DLSS quality setting did not apply");
                        hud->testClick("lens");
                        hud->testValue("lens-fov", 120);
                        break;
                    case 98:
                        require(renderer.experience.lens.fisheye && renderer.experience.lens.diagonalDegrees == 120,
                                "Neon lens/FOV controls failed");
                        hud->testClick("flashlight");
                        require(renderer.experience.flashlight, "Neon flashlight setting failed");
                        break;
                    case 100: case 102: case 104:
                        hud->testClick("environment");
                        require(renderer.experience.environment == (renderer.frame - 98) / 2,
                                "Neon lighting setting failed");
                        break;
                    case 105:
                        require(g.playerPosition().x == testStart.x && g.playerPosition().z == testStart.z,
                                "Neon player moved while the settings menu was open");
                        break;
                    case 106:
                        hud->testClick("environment");
                        hud->testClick("flashlight");
                        break;
                    case 108:
                        hud->testClick("lens");
                        hud->testValue("lens-fov", 90);
                        hud->testClick("dlss-quality");
                        break;
                    case 110: hud->testClick("view"); break;
                    case 112:
                        require(g.firstPerson, "Neon menu view setting failed");
                        hud->testClick("view");
                        break;
                    case 114:
                        hud->testClick("resume");
                        require(!g.paused, "Neon Resume button failed");
                        break;
                    case 120: key('R'); break;
                    case 121:
                        require(std::abs(g.playerPosition().z + 3) < .01f && std::abs(g.azimuth - XM_PI) < 1e-5f,
                                "Neon restart lost player spawn or camera direction");
                        break;
                    case 130: key(VK_ESCAPE); break;
                    case 135:
                        key(VK_ESCAPE);
                        require(!g.paused, "Neon Esc did not resume");
                        break;
                    case 148:
                        // FPS samples use a 500 ms wall-clock window, reset by
                        // the DLSS change at frame 108. Fast GPUs can reach the
                        // assertion before that window elapses. Leave one HUD
                        // update after the wait; do not change real play pacing.
                        Sleep(550);
                        break;
                    case 150:
                        require(hud->testText("render-fps").find("--") == std::string::npos &&
                                hud->testText("output-fps").find("--") == std::string::npos,
                                "Neon FPS counters never updated");
                        orbitStart = g.azimuth;
                        send(WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(300,300));
                        send(WM_MOUSEMOVE, MK_RBUTTON, MAKELPARAM(380,300));
                        break;
                    case 154:
                        send(WM_RBUTTONUP, 0, MAKELPARAM(380,300));
                        require(std::abs(g.azimuth - orbitStart) > .1f, "Neon orbit input failed");
                        orbitStart = g.distance;
                        send(WM_MOUSEWHEEL, MAKEWPARAM(0,WHEEL_DELTA));
                        require(g.distance != orbitStart, "Neon wheel zoom failed");
                        break;
                    case 165: key('R'); break;
                    case 179:
                        require(!g.won && !g.gateOpen && !g.paused && !g.firstPerson && renderer.dlssQuality() == 0,
                                "Neon controls did not return to playable state");
                        logLine("PASS: Neon keyboard rolling/jump, orbit/zoom, Tab, FPS, Esc pause/resume, DLSS, lens/FOV, lighting, flashlight and restart");
                        break;
                    }
                }
                renderer.beginSimulation();
                // A ready display event can win the wait while input is queued.
                // Consume it BEFORE simulation/camera sampling in this same frame.
                pumpMessages();
                if (!running)
                    break;
                applyResize();
                float rotation = angle + (options.animate ? .15f * std::sin(renderer.frame * .04f) : 0);
                if (options.opticalControlsTest) {
                    if (renderer.frame == 16)
                        key(VK_F4);
                    if (renderer.frame == 17)
                        require(renderer.opticalDebugMode() == 1, "F4 optical view failed");
                    if (renderer.frame == 24)
                        key(VK_F5);
                    if (renderer.frame == 25)
                        require(renderer.opticalBudgetsFrozen(), "F5 optical freeze failed");
                    if (renderer.frame == 28)
                        send(WM_SIZE, SIZE_RESTORED, MAKELPARAM(1600, 900));
                    if (renderer.frame == 32)
                        for (uint32_t i = 0; i < 6; ++i)
                            key(VK_F4);
                    if (renderer.frame == 33)
                        require(renderer.opticalDebugMode() == 0, "Optical view cycle failed");
                    if (renderer.frame == 36)
                        send(WM_SIZE, SIZE_RESTORED, MAKELPARAM(options.width, options.height));
                    if (renderer.frame == 40)
                        key(VK_F5);
                    if (renderer.frame == 41)
                        require(!renderer.opticalBudgetsFrozen(), "Optical unfreeze failed");
                    applyResize();
                }
                if (options.fluidRoomTest) {
                    auto &f = *renderer.fluid;
                    static uint32_t stoppedCount = 0;
                    auto clickValve = [&] {
                        auto p = hud->wallWaterButtonPoint();
                        auto l = MAKELPARAM(p.x, p.y);
                        send(WM_MOUSEMOVE, 0, l);
                        send(WM_LBUTTONDOWN, MK_LBUTTON, l);
                        send(WM_LBUTTONUP, 0, l);
                        require(hud->requestedWallWater, "Wall UI button did not receive its click");
                    };
                    switch (renderer.frame) {
                    case 0:
                        key('T');
                        require(f.emitter.enabled, "T did not open wall valve");
                        break;
                    case 30:
                        key('T');
                        stoppedCount = f.activeParticles;
                        break;
                    case 35:
                        require(f.activeParticles == stoppedCount && !f.emitter.enabled,
                                "Closed valve emitted water");
                        key('P');
                        key('T');
                        break;
                    case 45:
                        require(f.activeParticles == stoppedCount && f.paused, "Paused fluid emitted water");
                        key(VK_OEM_PERIOD);
                        break;
                    case 46:
                        require(f.activeParticles > stoppedCount, "Single step did not emit water");
                        break;
                    case 50:
                        key('P');
                        break;
                    case 70:
                        game->place(0, {-4.3f, .7f, 2.4f});
                        require(renderer.nearWallValve(*game), "Valve proximity did not follow player");
                        key('E');
                        require(!f.emitter.enabled, "E did not close nearby valve");
                        stoppedCount = f.activeParticles;
                        break;
                    case 75:
                        require(f.activeParticles == stoppedCount, "E-closed valve still emitted");
                        break;
                    case 80:
                        clickValve();
                        break;
                    case 81:
                        require(f.emitter.enabled, "UI did not open wall valve");
                        break;
                    case 100:
                        key('B');
                        break;
                    case 101:
                        require(f.activeParticles == options.fluidParticles && !f.emitter.enabled &&
                                    !f.emittedParticles,
                                "Reset did not clear inlet and added water");
                        break;
                    case 110:
                        clickValve();
                        break;
                    case 179:
                        require(f.emitter.enabled && f.activeParticles > options.fluidParticles,
                                "Final wall flow failed");
                        break;
                    }
                }
                if (options.frameGenerationTest) {
                    const auto &s = renderer.streamlineState();
                    if (renderer.frame == 40)
                        key(VK_ESCAPE);
                    if (renderer.frame == 42)
                        require(!s.fgEnabled, "Pause did not suspend Frame Generation");
                    if (renderer.frame == 48)
                        key(VK_ESCAPE);
                    if (renderer.frame == 52)
                        require(s.fgEnabled, "Resume did not restore Frame Generation");
                    if (renderer.frame == 70)
                        key(VK_F8);
                    if (renderer.frame == 72)
                        require(!s.fgLoaded && s.multiplier == 1, "FG Off retained presentation plugin");
                    if (renderer.frame == 85)
                        key(VK_F8);
                    if (renderer.frame == 88)
                        require(s.fgLoaded && s.fgEnabled, "FG re-enable failed");
                    if (renderer.frame == 110)
                        send(WM_SIZE, SIZE_RESTORED, MAKELPARAM(1600, 900));
                    if (renderer.frame == 130)
                        send(WM_SIZE, SIZE_RESTORED, MAKELPARAM(options.width, options.height));
                    if (renderer.frame == 160)
                        require(!s.fgStatus && s.interpolatedPresents > 30,
                                "No valid generated presents after resize");
                    // Injected messages follow the same mutation-before-render
                    // ordering as the normal pumpMessages/applyResize path.
                    applyResize();
                }
                if (waveFillTest) {
                    auto &f = *renderer.fluid;
                    auto &wave = *f.hamiltonian;
                    static double stoppedVolume = 0;
                    static uint64_t stoppedEmitted = 0;
                    if (renderer.frame) {
                        require(wave.steps == f.stepCount, "Fill advanced different wave/particle clocks");
                        require(std::abs(wave.publishedMeanHeight() - (f.description().minimum.y + wave.depth())) < 2e-5f,
                                "Rendered wave height lost the inlet volume ledger");
                    }
                    switch (renderer.frame) {
                    case 0:
                        if (!f.emitter.enabled)
                            key('T');
                        require(f.emitter.enabled, "T did not open the Hamiltonian inlet");
                        break;
                    case 20:
                        require(wave.freeWaterSamples() > 0, "Inlet did not emit simulated free water");
                        stoppedVolume = wave.addedVolume();
                        stoppedEmitted = f.emittedParticles;
                        key('P');
                        break;
                    case 24:
                        require(wave.addedVolume() == stoppedVolume && f.emittedParticles == stoppedEmitted,
                                "Paused water continued emitting or transferring mass");
                        key(VK_OEM_PERIOD);
                        break;
                    case 25:
                        require(f.emittedParticles > stoppedEmitted, "Single-step did not advance the source");
                        key('T');
                        stoppedVolume = wave.addedVolume();
                        stoppedEmitted = f.emittedParticles;
                        key(VK_OEM_PERIOD);
                        break;
                    case 26:
                        require(!f.emitter.enabled && f.emittedParticles == stoppedEmitted,
                                "Closed inlet emitted during a single step");
                        key('P');
                        key('T');
                        break;
                    case 60:
                        game->place(0, {f.emitter.position.x + .8f, options.fluidDepth + 1, f.emitter.position.z});
                        require(renderer.nearWallValve(*game), "Hamiltonian wall valve is unreachable");
                        key('E');
                        require(!f.emitter.enabled, "E did not close the Hamiltonian wall valve");
                        stoppedVolume = wave.addedVolume();
                        stoppedEmitted = f.emittedParticles;
                        break;
                    case 65:
                        require(f.emittedParticles == stoppedEmitted, "E-closed inlet kept emitting");
                        hud->testClick("wall-water");
                        break;
                    case 66:
                        require(f.emitter.enabled, "Wall UI did not open the Hamiltonian inlet");
                        break;
                    case 85:
                        require(f.emittedParticles > stoppedEmitted && wave.freeWaterSamples() > 0,
                                "Reopened inlet failed to emit water");
                        key('B');
                        break;
                    case 86:
                        require(wave.addedVolume() == 0 && wave.depth() == options.fluidDepth &&
                                    !f.emitter.enabled && !f.emitterFull, "Reset did not drain Hamiltonian inlet volume");
                        break;
                    case 90:
                        key('T');
                        game->place(0, {0, options.fluidDepth + 1, 3.2f});
                        break;
                    }
                    if (renderer.frame == options.frames - 1) {
                        require(wave.addedVolume() > 0 && wave.regionChanges() > 0,
                                "Filling did not survive adaptive region changes and reset");
                        if (f.emitterFull) {
                            require(f.emitterFull && wave.full() && !f.emitter.enabled,
                                    "Hamiltonian capacity did not close the inlet");
                            key('T');
                            require(!f.emitter.enabled, "T reopened a full Hamiltonian reservoir");
                        }
                        logLine("PASS Hamiltonian inlet: T/E/UI, pause, step, drain/reset, adaptive regions and fill limit");
                    }
                }
                if (oceanControlsTest) {
                    const auto f = renderer.frame;
                    if (f == 5 || f == 20) {
                        key('Y');
                        require(renderer.experience.environment == 1, "Y did not select night");
                    }
                    if (f == 10 || f == 30) {
                        hud->testClick(f == 10 ? "ocean-time" : "environment");
                        require(renderer.experience.environment == 0, "Ocean lighting button did not select day");
                    }
                    if (f == 40 || f == 45) {
                        key('I');
                        require(renderer.inspectingFluid() == (f == 40), "Ocean overview did not toggle");
                    }
                    if (f == 50 || f == 55) {
                        key(VK_TAB);
                        require(renderer.experience.firstPerson == (f == 50), "Ocean player view did not toggle");
                    }
                    if (f == 60 || f == 110) key('T');
                    if (f == 119) {
                        require(renderer.fluid->stepCount >= 200, "Sky controls reset or stopped water");
                        require(renderer.fluid->emittedParticles > 0, "Ocean outlet did not emit water");
                        logLine("PASS ocean: day/night, both lighting buttons, overview, player view and outlet");
                    }
                }
                if (options.oceanSwimTest) {
                    const auto f=renderer.frame;
                    if(f==0) {
                        renderer.experience.ballFloats=false;game->setBallFloating(false);
                        game->place(0,{30,.75f,30});game->distance=5;game->elevation=.20f;
                    }
                    if(f==10)send(WM_KEYDOWN,'W');
                    if(f==50)send(WM_KEYUP,'W');
                    if(f==60){swimStartHeight=game->playerPosition().y;send(WM_KEYDOWN,VK_SPACE);}
                    if(f==180) {
                        require(game->playerPosition().y>swimStartHeight+1.4f,"GPU water did not allow sustained Space ascent");
                        require(game->jumps==0&&!game->ballFloats(),"Underwater propulsion jumped or changed density");
                        swimStartHeight=game->playerPosition().y;send(WM_KEYUP,VK_SPACE);
                    }
                    if(f==240) {
                        require(game->playerPosition().y<swimStartHeight-.4f,"Key release did not resume sinking");
                        game->clearInput();renderer.experience.ballFloats=true;game->setBallFloating(true);
                        game->place(game->boatBody,{40,6.4f,40});game->place(0,{40,7.4f,38});
                        require(game->toggleBoat(),"Ocean voyage could not board the boat");
                        boatTestStart={40,6.4f,40};game->distance=12;game->elevation=.4f;
                        send(WM_KEYDOWN,'W');
                    }
                    if(f==540)send(WM_KEYDOWN,'D');
                    if(f>330) {
                        auto hull=game->poses().at(size_t(game->boatBody)+1);
                        if(hull._22<.3f)throw std::runtime_error("Ocean boat capsized at frame "+std::to_string(f));
                    }
                    if(f==599) {
                        auto p=game->playerPosition();
                        require(std::hypot(p.x-boatTestStart.x,p.z-boatTestStart.z)>12,"Larger boat failed sustained ocean propulsion");
                        logLine("PASS ocean: floor traversal, no air pocket, held Space, key release and powered voyage");
                    }
                }
                if (wavePatchTest) {
                    auto &f = *renderer.fluid;
                    const float t = std::clamp((float(renderer.frame) - 30.f) / 150.f, 0.f, 1.f);
                    game->place(0, {8.f * t, 2.2f, 3.2f + 7.f * t});
                    if (renderer.frame == 239) {
                        const auto &d = f.description();
                        require(f.hamiltonian->regionChanges() > 20 && f.hamiltonian->activeColumns() > 0,
                                "Hamiltonian regions did not respond to moving bodies");
                        require(d.minimum.x + 1.9f < 8.f && d.maximum.x - 1.9f > 8.f &&
                                    d.minimum.z + 1.9f < 10.2f && d.maximum.z - 1.9f > 10.2f,
                                "Moving body left the addressable adaptive basin");
                        if (game->boatBody >= 0) {
                            const auto poses = game->poses();
                            require(poses[game->boatBody + 1]._42 > lab::largeWater::depth - .6f &&
                                        game->submergedBoat > 0,
                                    "Boat lost buoyancy while another region moved");
                        }
                    }
                }
                if (options.fluidControlTest || waveControlsTest) {
                    auto &f = *renderer.fluid;
                    if (waveControlsTest) {
                        require(f.hamiltonian->steps == f.stepCount,
                                "Wave and 3D controls advanced different clocks");
                        if (renderer.frame == 0)
                            f.debugVisible = true;
                    }
                    if (f.cutCells && renderer.frame < 2) {
                        require(f.cutCells->debugVisible == (renderer.frame == 1),
                                "Cut-cell view did not persist");
                        key('K');
                        require(f.cutCells->debugVisible == (renderer.frame == 0),
                                "Cut-cell K view toggle failed");
                    }
                    if (f.bulk && renderer.frame < 3) {
                        require(f.bulk->debugMode == renderer.frame, "Bulk debug cycle did not persist");
                        key(VK_F9);
                        require(f.bulk->debugMode == (renderer.frame + 1) % 3, "Bulk F9 view cycle failed");
                    }
                    switch (renderer.frame) {
                    case 0:
                        key('P');
                        break;
                    case 1:
                        require(f.stepCount == 0 && f.paused, "Fluid pause failed");
                        key(VK_OEM_PERIOD);
                        break;
                    case 2:
                        require(f.stepCount == 1, "Fluid single-step failed");
                        break;
                    case 3:
                        require(f.stepCount == 1, "Paused fluid continued advancing");
                        key('B');
                        break;
                    case 4:
                        require(f.stepCount == 0, "Fluid reset failed");
                        key('P');
                        break;
                    case 5:
                        require(f.stepCount == 2 && !f.paused, "Fluid resume failed");
                        key('P');
                        key('G');
                        break;
                    case 6:
                        require(f.stepCount == 2 && f.debugMode == 1, "Fluid grid view failed");
                        key('G');
                        break;
                    case 7:
                        require(f.debugMode == 2, "Fluid pressure view failed");
                        key('G');
                        key('V');
                        break;
                    case 8:
                        require(f.debugMode == 3 && !f.debugVisible, "Fluid overlay toggle failed");
                        key('V');
                        key('G');
                        break;
                    case 9:
                        require(f.debugMode == 4 && f.debugVisible, "Fluid classification view failed");
                        key('G');
                        break;
                    case 10:
                        require(f.debugMode == 0, "Fluid debug view wrap failed");
                        key(VK_OEM_PERIOD);
                        break;
                    case 11:
                        require(f.stepCount == 3, "Fluid second single-step failed");
                        break;
                    }
                }
                float cameraAzimuth = azimuth;
                if (options.fluidComplexityControlsTest) {
                    auto &c = *renderer.fluidComplexity;
                    if (renderer.frame == 0) {
                        key(VK_F6);
                        require(c.debugMode == 1, "Complexity F6 view failed");
                    } else if (renderer.frame == 1) {
                        key(VK_F7);
                        require(c.frozen, "Complexity F7 freeze failed");
                    } else if (renderer.frame == 4) {
                        key('B'); // Reset must invalidate frozen decisions.
                    } else if (renderer.frame == 6) {
                        key(VK_F7);
                        require(!c.frozen, "Complexity unfreeze failed");
                    } else if (renderer.frame >= 7) {
                        key(VK_F6);
                        if (renderer.frame == 15) {
                            key(VK_F6);
                            require(c.debugMode == 0, "Complexity view wrap failed");
                        }
                    }
                }
                if (options.fluidInteriorCycle) {
                    if (renderer.frame == 60) {
                        key('P');
                        key(VK_F10);
                        require(renderer.fluid->paused && renderer.fluid->interior->forcedFine,
                                "Interior forced-fine control failed");
                    } else if (renderer.frame == 65) {
                        require(!renderer.fluid->changedThisFrame, "Paused interior invalidated the surface");
                        key(VK_OEM_PERIOD);
                    } else if (renderer.frame == 70) {
                        key('P');
                    }
                }
                if (options.fluidSurfaceControlsTest) {
                    auto &f = *renderer.fluid;
                    auto &s = *renderer.fluidSurface;
                    switch (renderer.frame) {
                    case 0:
                        key('P');
                        key('I');
                        require(f.paused && renderer.inspectingFluid(), "Fluid inspection input failed");
                        break;
                    case 1:
                        require(f.stepCount == 0, "Inspection pause advanced fluid");
                        key('N');
                        require(s.debugMode == 1, "Fluid normal view failed");
                        break;
                    case 2:
                        key('N');
                        require(s.debugMode == 2, "Fluid brick view failed");
                        break;
                    case 3:
                        key('N');
                        require(s.debugMode == 3, "Fluid motion view failed");
                        break;
                    case 4:
                        key('N');
                        require(s.debugMode == 0, "Fluid shaded view failed");
                        key('I');
                        require(!renderer.inspectingFluid(), "Return to ball camera failed");
                        break;
                    case 5:
                        key(VK_OEM_PERIOD);
                        break;
                    case 6:
                        require(f.stepCount == 1, "Rendered fluid single step failed");
                        key('P');
                        break;
                    case 7:
                        require(f.stepCount == 3, "Rendered fluid resume failed");
                        key('V');
                        require(f.debugVisible, "Rendered solver overlay failed");
                        break;
                    case 8:
                        key('V');
                        require(!f.debugVisible, "Rendered solver overlay hide failed");
                        break;
                    case 9:
                        key('P');
                        key('B');
                        break;
                    case 10:
                        require(f.stepCount == 0 && f.paused, "Rendered fluid reset failed");
                        key('I');
                        break;
                    case 11:
                        require(renderer.inspectingFluid(), "Final fluid inspection failed");
                        break;
                    }
                }
                if (options.temporalTest) {
                    if (renderer.frame >= 64 && renderer.frame < 96)
                        rotation += .01f * float(renderer.frame - 63);
                }
                auto now = std::chrono::steady_clock::now();
                float dt = automation ? 1.f / 60
                                      : std::clamp(std::chrono::duration<float>(now - previousTime).count(),
                                                   .0001f, .05f);
                previousTime = now;
                if (options.profileFluidBursts) {
                    if (renderer.frame == 0)
                        renderer.fluid->emitter.enabled = false;
                    if (renderer.frame == 90)
                        renderer.fluid->emitter.enabled = true;
                    if (renderer.frame == 180)
                        game->place(0, options.fluidDeepPool ? XMFLOAT3{-20, options.fluidDepth + 2, 9.6f}
                                                             : XMFLOAT3{-1.5f, 2.f, 1.f});
                }
                if (game) {
                    if (options.experienceTest) {
                        const auto f = renderer.frame;
                        auto &settings = renderer.experience;
                        if (f == 2) {
                            key(VK_ESCAPE);
                            require(game->paused, "Esc did not open settings");
                        }
                        if (f == 4) {
                            require(!game->ballFloats() && game->ballDensity() > 2000,
                                    "Sink menu did not update physical density");
                            hud->testClick("environment");
                            require(settings.environment == 1, "Environment menu failed");
                        }
                        if (f == 6) {
                            require(game->ballFloats() && std::abs(game->ballMass() - 60) < .001f,
                                    "Float menu did not restore physical mass");
                            hud->testClick("flashlight");
                            require(settings.flashlight, "Flashlight menu failed");
                        }
                        if (f == 8) {
                            hud->testClick("view");
                            require(settings.firstPerson, "View menu failed");
                        }
                        if (f == 3 || f == 5) {
                            hud->testClick("ball-density");
                            require(settings.ballFloats == (f == 5), "Ball density menu failed");
                        }
                        if (f == 10) {
                            hud->testValue("lens-fov", 120);
                            require(settings.lens.diagonalDegrees == 120, "Fisheye FOV range failed");
                        }
                        if (f == 12 || f == 14)
                            hud->testClick("lens");
                        if (f == 18)
                            key(VK_ESCAPE);
                        if (f == 24) {
                            settings.environment = 0;
                            auto boat = game->poses().at(size_t(game->boatBody) + 1);
                            game->place(0, {boat._41, .72f, boat._43 - 2});
                            key('E');
                            require(game->piloting, "Boat interaction did not board");
                            boatTestStart = {boat._41, boat._42, boat._43};
                            settings.firstPerson = false;
                            game->input.forward = true;
                        }
                        if (f > 24 && f < 110) {
                            boatWet = boatWet || game->submergedBoat > .01f;
                            auto boat = game->poses().at(size_t(game->boatBody) + 1);
                            boatMoved = boatMoved || std::hypot(boat._41 - boatTestStart.x,
                                                                boat._43 - boatTestStart.z) > .2f;
                        }
                        if (f == 80)
                            game->input.right = true;
                        if (f == 105)
                            game->clearInput();
                        if (f == 110) {
                            key('E');
                            require(!game->piloting, "Boat interaction did not exit");
                            require(boatWet && boatMoved, "GPU water samples/boat propulsion failed");
                        }
                        if (f == 115) {
                            game->place(0, {-.5f, .68f, 2.4f});
                            settings.firstPerson = true;
                            game->input.dive = true;
                        }
                        if (f == 140)
                            require(renderer.viewCamera().position.y < .85f, "Underwater camera was raised");
                        if (f == 150 || f == 155) {
                            if (f == 150)
                                key(VK_ESCAPE);
                            hud->testValue("particle-limit", f == 150 ? 600.f : 500.f);
                            hud->testValue("grid-resolution", f == 150 ? 20.f : 12.f);
                            hud->testValue("simulation-rate", f == 150 ? 90.f : 120.f);
                            hud->testClick("apply-water");
                        }
                        if (f == 153 || f == 158) {
                            const auto &desc = renderer.fluid->description();
                            require(desc.maxParticles == (f == 153 ? 600000u : 500000u),
                                    "Particle limit not applied");
                            require(std::abs(desc.gridCellSize - (f == 153 ? .2f : .12f)) < 1e-5,
                                    "Simulation cell size not applied");
                            require(desc.simulationRate == (f == 153 ? 90.f : 120.f),
                                    "Simulation Hz not applied");
                        }
                        if (f == 160)
                            key(VK_ESCAPE);
                        if (f == 170)
                            settings.environment = 3;
                        if (f == 190) {
                            settings.environment = 0;
                            settings.lens.fisheye = false;
                        }
                        if (f == 220 || f == 230)
                            key(VK_ESCAPE);
                    }
                    if (hud->requestedWallWater) {
                        renderer.toggleWallWater();
                        hud->requestedWallWater = false;
                    }
                    if (game->paused || game->won || game->tuning >= 0 || game->resetHistory)
                        orbitInput.clear();
                    else
                        orbitInput.step(dt, game->azimuth, game->elevation, true);
                } else {
                    orbitInput.step(dt, azimuth, elevation, false);
                    cameraAzimuth = azimuth;
                    if (options.temporalTest && renderer.frame >= 160 && renderer.frame < 192)
                        cameraAzimuth += .003f * float(renderer.frame - 159);
                }
                if (game) {
                    if (options.orbitTest)
                        game->azimuth = .42f + float(renderer.frame) * .006f;
                    if (demoTour) {
                        // Drive the normal player controls; do not teleport the
                        // ball, override body velocities, or manufacture waves.
                        const float t = float(renderer.frame) / 60.f;
                        game->azimuth = .42f + .25f * std::sin(t * .25f);
                        game->elevation = .62f + .08f * std::sin(t * .2f);
                        const auto p = game->playerPosition();
                        const float dx = 2.4f * std::sin(t * .8f) - p.x;
                        const float dz = 3.f + 1.2f * std::cos(t * .8f) - p.z;
                        const float ahead = -std::sin(game->azimuth) * dx - std::cos(game->azimuth) * dz;
                        const float side = std::cos(game->azimuth) * dx - std::sin(game->azimuth) * dz;
                        game->input.forward = ahead > .25f;
                        game->input.back = ahead < -.25f;
                        game->input.right = side > .25f;
                        game->input.left = side < -.25f;
                        game->input.jump = renderer.frame % 180 == 90;
                    }
                    if (options.fluidTemporalTest) {
                        const uint32_t f = renderer.frame;
                        // First settle, then isolate camera motion over frozen liquid.
                        // Resume liquid at frame 224; finally roll at the steep angle.
                        renderer.fluid->paused = f >= 96 && f < 224;
                        game->azimuth = .42f + .006f * float(f > 128 ? f - 128 : 0);
                        game->elevation = .56f + .84f * std::clamp((float(f) - 128) / 48.f, 0.f, 1.f);
                        game->input.forward = f >= 272 && f < 296;
                    }
                    if (options.rollingTest) {
                        game->input.forward = renderer.frame < 120 || renderer.frame >= 240;
                        game->input.back = !game->input.forward;
                        game->input.jump = renderer.frame == 170;
                    }
                    if (options.gameplayTest) {
                        auto &g = *game;
                        switch (renderer.frame) {
                        case 15:
                            testStart = g.playerPosition();
                            movementRrResets = renderer.rrHistoryResets;
                            send(WM_KEYDOWN, 'D');
                            break;
                        case 45:
                            send(WM_KEYUP, 'D');
                            require(renderer.rrHistoryResets == movementRrResets,
                                    "Rolling reset global RR history");
                            require(std::hypot(g.playerPosition().x - testStart.x,
                                               g.playerPosition().z - testStart.z) > 1,
                                    "WASD message test failed");
                            key(VK_SPACE);
                            break;
                        case 48:
                            require(g.jumps == 1, "Jump input test failed");
                            key(VK_SPACE);
                            break;
                        case 50:
                            require(g.jumps == 1, "Air jump input was accepted");
                            orbitStart = g.azimuth;
                            send(WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(300, 300));
                            send(WM_MOUSEMOVE, MK_RBUTTON, MAKELPARAM(380, 300));
                            break;
                        case 52:
                            require(g.azimuth < orbitStart - .35f, "Orbit input was not integrated");
                            send(WM_RBUTTONUP, 0, MAKELPARAM(380, 300));
                            break;
                        case 58:
                            require(std::abs(g.azimuth - (orbitStart - .4f)) < .0001f,
                                    "Orbit smoothing lost or duplicated displacement");
                            key('L');
                            require(renderer.laserWavelength == 450, "Laser blue selection failed");
                            key('L');
                            require(renderer.laserWavelength == 638, "Laser red selection failed");
                            key('L');
                            require(renderer.laserWavelength == 532, "Laser green selection failed");
                            break;
                        case 60:
                            g.load(0);
                            key('F');
                            require(g.tuning == 1, "F did not lock prism");
                            break;
                        case 100:
                            require(g.tuningReady(), "Top-down transition did not finish");
                            send(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(viewWidth / 2, viewHeight / 2));
                            send(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(viewWidth / 2 + 8, viewHeight / 2));
                            send(WM_LBUTTONUP, 0, MAKELPARAM(viewWidth / 2 + 8, viewHeight / 2));
                            send(WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA));
                            break;
                        case 102:
                            require(g.tuneMoves > 0 && g.tuneTurns > 0,
                                    "Mouse/scroll precision controls failed");
                            key(VK_RETURN);
                            require(g.tuning < 0, "Enter did not save tuning");
                            key('E');
                            require(g.held == 1, "E did not grab prism");
                            key('X');
                            require(g.held < 0 && g.throws > 0, "X did not throw prism");
                            break;
                        case 110:
                            g.load(0);
                            g.azimuth = 0;
                            g.place(0, {-3.2f, .7f, 3.4f});
                            key('E');
                            require(g.held == 2, "Cube grab input failed");
                            send(WM_LBUTTONDOWN, 0);
                            send(WM_LBUTTONUP, 0);
                            require(g.held < 0, "Mouse throw failed");
                            break;
                        case 120:
                            key('R');
                            key('U');
                            require(g.ui, "U did not expose UI");
                            key('U');
                            key(VK_ESCAPE);
                            require(g.paused, "Esc did not pause");
                            key(VK_ESCAPE);
                            require(!g.paused, "Esc did not resume");
                            g.place(1, {0, 1.60f, 0}, XM_PI / 6);
                            require(g.dock(1), "Solution fixture failed to lock prism");
                            break;
                        case 300:
                            require(g.gateOpen, "Real photon receiver did not unlock gate");
                            g.place(0, {0, .7f, -6.6f});
                            break;
                        case 320:
                            require(g.won, "Portal crossing did not complete chamber");
                            break;
                        }
                    }
                    if (options.fluidMacCycle) {
                        auto &m = *renderer.fluid->mac;
                        if (renderer.frame == 40 || renderer.frame == 70) {
                            key(VK_F10);
                            require(m.forcedFine == (renderer.frame == 40), "MAC force-fine input failed");
                        }
                        if (renderer.frame == 41 || renderer.frame == 69)
                            require(m.coarseLeaves() == 0, "MAC force-fine did not restore the grid");
                        if (renderer.frame == 30 || renderer.frame == 60) {
                            key(VK_F11);
                            require(m.debugVisible == (renderer.frame == 30), "MAC LOD overlay input failed");
                        }
                        if (renderer.frame == 39 || renderer.frame == 119)
                            require(m.coarseLeaves() > 0, "MAC did not return to adaptive pressure leaves");
                    }
                    // Freeze all rigid poses while checking fluid pause/single-step.
                    if (!(options.fluidInteriorCycle && renderer.frame >= 60 && renderer.frame < 70))
                        game->step(dt);
                    if (options.fluidInteriorWake) {
                        const float t = std::clamp((float(renderer.frame) - 60) / 120, 0.f, 1.f);
                        game->place(0, {1.f + 4.5f * t, 2.7f, -3.1f});
                    }
                    hud->laserWavelength = renderer.laserWavelength;
                    const auto &sl = renderer.streamlineState();
                    hud->frameMultiplier = sl.multiplier;
                    hud->maxFrameMultiplier = sl.fgSupported ? sl.maxMultiplier : 1;
                    hud->frameGenActive = sl.fgEnabled;
                    hud->renderedFrames = renderer.frame;
                    hud->presentedFrames = sl.presentedFrames;
                    hud->dlssQuality = renderer.dlssQuality();
                    hud->renderWidth = sl.renderWidth;
                    hud->renderHeight = sl.renderHeight;
                    hud->outputWidth = sl.width;
                    hud->outputHeight = sl.height;
                    hud->frameGenStatus = sl.fgStatus;
                    hud->frameGenMinimumDimension = sl.minimumDimension;
                    hud->frameGenReason = sl.fgReason;
                    if (renderer.fluid) {
                        hud->fluidRoom = renderer.roomLiquid();
                        hud->wallWater = renderer.fluid->emitter.enabled;
                        hud->waterFull = renderer.fluid->emitterFull;
                        hud->nearbyValve = renderer.nearWallValve(*game);
                        if (renderer.fluid->hamiltonian) {
                            const auto &wave = *renderer.fluid->hamiltonian;
                            std::ostringstream fillStatus;
                            if (renderer.fluid->paused)
                                fillStatus << "Paused · ";
                            fillStatus << std::fixed << std::setprecision(3) << wave.depth() << " m · +"
                                       << std::setprecision(1) << renderer.fluid->emittedParticles * double(renderer.fluid->particleVolume)
                                       << " m³ supplied";
                            hud->waterFillStatus = fillStatus.str();
                        }
                        const char *fluidViews[] = {"particles",
                                                    "MAC velocity",
                                                    "pressure",
                                                    "divergence",
                                                    "liquid cells",
                                                    "particle mass (blue: 1x, red: 8x)",
                                                    "coarse interior ownership (cyan)",
                                                    "GPU work (cyan: P2G, orange: density, white: both)"};
                        std::ostringstream status;
                        const auto *resampling = renderer.fluid->resampling.get();
                        status << std::fixed << std::setprecision(2)
                               << (renderer.fluid->hamiltonian ? renderer.fluid->hamiltonian->activeSamples()
                                   : resampling ? resampling->activeSamples() : renderer.fluid->activeParticles)
                               << " samples / " << renderer.fluid->simulationMs << " ms simulation";
                        if (renderer.fluid->hamiltonian)
                            status << " / depth " << renderer.fluid->hamiltonian->depth() << " m"
                                   << " / +" << renderer.fluid->hamiltonian->addedVolume() << " m³ filled";
                        if (resampling)
                            status << " / " << renderer.fluid->activeParticles << " rest-mass units / "
                                   << resampling->milliseconds() << " ms resampling";
                        if (renderer.fluidSurface)
                            status << " / " << renderer.fluidSurface->reconstructionMs << " ms surface / "
                                   << renderer.fluidSurface->blasMs << " ms BLAS / "
                                   << renderer.fluidSurface->surfaceBricks << " surface bricks";
                        if (renderer.fluid->paused)
                            status << " / PAUSED";
                        if (renderer.fluid->interior)
                            status << " / " << renderer.fluid->interior->massUnits()
                                   << " coarse-owned mass / F10: "
                                   << (renderer.fluid->interior->forcedFine ? "forced fine"
                                                                            : "automatic interior");
                        if (renderer.fluid->pressureSolver)
                            status << " / " << renderer.fluid->pressureSolver->gpuMs << " ms pressure ("
                                   << options.fluidPressure << ')';
                        if (renderer.fluid->mac)
                            status << " / " << renderer.fluid->mac->gpuMs << " ms mixed MAC / "
                                   << renderer.fluid->mac->coarseLeaves()
                                   << " coarse cells / F10 force fine, F11 LOD view";
                        if (renderer.fluid->bulk)
                            status << " / " << renderer.fluid->bulk->gpuMs
                                   << " ms passive bulk / F9: inventory view "
                                   << renderer.fluid->bulk->debugMode;
                        if (renderer.fluid->cutCells)
                            status << " / K: solid cut-cell view "
                                   << (renderer.fluid->cutCells->debugVisible ? "on" : "off");
                        if (renderer.whitewater)
                            status << " / " << renderer.whitewater->counts[0] << " foam / "
                                   << renderer.whitewater->counts[1] << " bubbles / "
                                   << renderer.whitewater->counts[2] << " spray / "
                                   << renderer.whitewater->simulationMs + renderer.whitewater->blasMs
                                   << " ms whitewater";
                        if (renderer.fluid->debugVisible)
                            status << " / " << fluidViews[renderer.fluid->debugMode];
                        if (renderer.fluidComplexity) {
                            const auto &c = *renderer.fluidComplexity;
                            status << " / importance " << c.classificationMs + c.schedulingMs << " ms"
                                   << (renderer.fluid->mac ? " / requested importance LOD: "
                                                           : " / requested LOD (MAC uniform): ")
                                   << c.viewName() << (c.frozen ? " FROZEN" : "") << " / F6 view, F7 freeze";
                            hud->complexityStatus =
                                std::string("F6: ") + c.viewName() + (c.frozen ? " [FROZEN]" : "") +
                                (renderer.fluid->mac ? " · F7: importance freeze · F10: force fine · F11: "
                                                       "actual MAC LOD · U: details"
                                                     : " · F7: freeze · blue: low/coarse, red: high/fine · "
                                                       "resolution remains uniform · U: details");
                            if (renderer.fluidSurface && renderer.fluidSurface->adaptive)
                                hud->complexityStatus =
                                    "Surface LOD enabled · sampling view: orange fine / blue coarse · "
                                    "F6/F7: importance · U: details";
                        }
                        hud->fluidStatus = status.str();
                    }
                    hud->update(dt, renderer.receiverWatts);
                    if (hud->quit)
                        break;
                }
                renderer.endSimulation();
                renderer.render(rotation, cameraAzimuth, elevation, game.get(), hud.get(), dt);
                if (options.samplingControlsTest && options.capture &&
                    (renderer.frame == 18 || renderer.frame == 26 || renderer.frame == 34 || renderer.frame == 62)) {
                    const auto prefix = folder / (name + "-sampling-" + std::to_string(renderer.frame));
                    renderer.capture(prefix);
                    renderer.report(prefix.string() + ".json");
                }
                if (options.neonControlsTest && options.capture &&
                    (renderer.frame == 96 || renderer.frame == 105 || renderer.frame == 132)) {
                    const auto prefix = folder / (name + "-menu-" + std::to_string(renderer.frame));
                    renderer.capture(prefix);
                    renderer.report(prefix.string() + ".json");
                }
                if (dlssSettingsTest && options.capture &&
                    (renderer.frame == 32 || renderer.frame == 80 || renderer.frame == 128 ||
                     renderer.frame == 176 || renderer.frame == 182)) {
                    const auto prefix = folder / (name + "-settings-" + std::to_string(renderer.frame));
                    renderer.capture(prefix);
                    renderer.report(prefix.string() + ".json");
                }
                if (options.experienceTest &&
                    (renderer.frame == 17 || renderer.frame == 104 || renderer.frame == 140 ||
                     renderer.frame == 186 || renderer.frame == 222)) {
                    renderer.capture(folder / (name + "-experience-" + std::to_string(renderer.frame)));
                    renderer.report(folder /
                                    (name + "-experience-" + std::to_string(renderer.frame) + ".json"));
                    const auto p = game->poses().at(size_t(game->boatBody) + 1);
                    logLine("Boat checkpoint " + std::to_string(renderer.frame) + " position " +
                            std::to_string(p._41) + "," + std::to_string(p._42) + "," +
                            std::to_string(p._43) + " upright " + std::to_string(p._22));
                }
                if (options.frameGenerationTest && options.capture && renderer.frame == 44) {
                    renderer.capture(folder / (name + "-pause"));
                    renderer.report(folder / (name + "-pause.json"));
                }
                if (options.oceanSwimTest && options.capture &&
                    (renderer.frame==55 || renderer.frame==175))
                    renderer.capture(folder / (name + (renderer.frame==55 ? "-floor" : "-ascent")));
                if (options.rollingTest)
                    require(renderer.rrHistoryResets == 1, "Rolling/jumping reset global RR history");
                if (options.opticalEstimatorTest && renderer.frame >= 128 && renderer.frame % 16 == 0) {
                    auto prefix = folder / (name + "-" + std::to_string(renderer.frame));
                    renderer.capture(prefix);
                    renderer.report(prefix.string() + ".json");
                }
                if (options.fluidTemporalTest) {
                    require(renderer.rrHistoryResets == 1, "Water camera motion reset RR history");
                    const uint32_t f = renderer.frame;
                    if ((f >= 125 && f <= 128) || (f >= 173 && f <= 176) || (f >= 221 && f <= 224) ||
                        (f >= 269 && f <= 272) || f >= 317) {
                        auto prefix = folder / (name + "-" + std::to_string(f));
                        renderer.capture(prefix);
                        renderer.report(prefix.string() + ".json");
                    }
                }
                if (options.temporalTest) {
                    require(renderer.rrHistoryResets == 1, "Continuous motion reset global RR history");
                    if (renderer.frame == 64 || renderer.frame == 96 || renderer.frame == 160 ||
                        renderer.frame == 192 || renderer.frame >= 253) {
                        auto prefix = folder / (name + "-" + std::to_string(renderer.frame));
                        renderer.capture(prefix);
                        renderer.report(prefix.string() + ".json");
                    }
                }
                if (game) {
                    LaserResult signal{};
                    signal.stats.y = .35f * renderer.receiverWatts / lab::receiverThreshold;
                    signal.sensors[0].x = renderer.receiverWatts;
                    game->receive(signal, dt);
                }
                // The HUD already owns live FPS. Repainting the OS caption every
                // 15 frames adds periodic main-thread work during smooth orbit.
                if (renderer.frame == 15)
                    SetWindowTextW(window, options.neonNight
                        ? L"NOCTURNE | WASD: roll · Space: jump · Right drag: look · Esc: settings"
                        : L"NVMatrixEngine | Fluid Lab | WASD: roll · F: tune · Esc: pause");
                const auto wholeFrameEnd = std::chrono::steady_clock::now();
                renderer.finishFrameProfile(
                    std::chrono::duration<double, std::milli>(wholeFrameEnd - wholeFrameStart).count(),
                    std::chrono::duration<double, std::milli>(wholeFrameEnd - applicationStart).count());
            }
            if (options.frames && renderer.frame != options.frames)
                throw std::runtime_error("Bounded validation was interrupted before completion");
            if (options.capture)
                renderer.capture(folder / name);
            renderer.report(folder / (name + ".json"));
            if (game) {
                if (options.gameplayTest)
                    require(game->won, "Gameplay test did not finish");
                std::ofstream report(folder / (name + ".game.json"));
                report << "{\"won\":" << (game->won ? "true" : "false")
                       << ",\"gateOpen\":" << (game->gateOpen ? "true" : "false")
                       << ",\"charge\":" << game->charge << ",\"jumps\":" << game->jumps
                       << ",\"throws\":" << game->throws << ",\"tuneMoves\":" << game->tuneMoves
                       << ",\"tuneTurns\":" << game->tuneTurns
                       << ",\"ballFloats\":" << (game->ballFloats() ? "true" : "false")
                       << ",\"ballMassKg\":" << game->ballMass()
                       << ",\"ballDensityKgM3\":" << game->ballDensity()
                       << ",\"ballHeightM\":" << game->playerPosition().y
                       << ",\"pose\":\"" << options.pose << "\"}\n";
            }
            activeGame = nullptr;
            activeHud = nullptr;
            activeRenderer = nullptr;
        }
        DestroyWindow(window);
        return 0;
    } catch (const lab::StartupCancelled &) {
        activeStartup = nullptr;
        activeGame = nullptr;
        activeHud = nullptr;
        activeRenderer = nullptr;
        logLine("Startup: canceled before gameplay");
        if (window) DestroyWindow(window);
        return 0;
    } catch (const std::exception &e) {
        activeStartup = nullptr;
        activeGame = nullptr;
        activeHud = nullptr;
        activeRenderer = nullptr;
        logLine(std::string("LAB ERROR: ") + e.what());
        std::ofstream("lab-error.txt") << e.what() << '\n';
        if (window)
            DestroyWindow(window);
        if (!automation) {
            const std::string message = std::string("NVMatrixEngine Lab could not continue:\n\n") + e.what() +
                                        "\n\nDetails were written to NVMatrixEngine.log and lab-error.txt "
                                        "beside NVMatrixFluidLab.exe.";
            MessageBoxA(nullptr, message.c_str(), "NVMatrixEngine Lab error", MB_OK | MB_ICONERROR);
        }
        return 1;
    }
}
