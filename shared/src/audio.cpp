#define MA_NO_ENCODING
#define MA_NO_FLAC
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#include "audio.h"
#include "logging.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <random>
#include <vector>
#include <stdexcept>
namespace {
constexpr int musicTrackCount = 5;
constexpr int nextMusicIndex(int current) {
    return (current + 1) % musicTrackCount;
}
template <class Generator> int randomMusicStart(Generator &random) {
    return std::uniform_int_distribution<int>(0, musicTrackCount - 1)(random);
}
} // namespace
struct Audio::Impl {
    ma_engine engine{};
    ma_sound music{}, effects[size_t(Sound::Count)]{};
    std::array<bool, size_t(Sound::Count)> loaded{};
    std::array<float, size_t(Sound::Count)> cooldown{};
    std::filesystem::path folder;
    bool ready = false, musicReady = false;
    int index = -1;
    float volume = 0, effectVolume = .5f;
};
Audio::Audio(const std::filesystem::path &folder, bool silent) : impl(std::make_unique<Impl>()) {
    impl->folder = folder;
    if (silent)
        return;
    auto config = ma_engine_config_init();
    config.channels = 2;
    config.sampleRate = 48000;
    if (ma_engine_init(&config, &impl->engine) != MA_SUCCESS) {
        logLine("Audio device unavailable; continuing silently.");
        return;
    }
    impl->ready = true;
    const char *names[] = {"tick", "rotate", "dock", "grab", "throw", "jump", "unlock", "click", "victory"};
    for (size_t i = 0; i < impl->loaded.size(); i++) {
        auto path = (folder / (std::string(names[i]) + ".wav")).wstring();
        impl->loaded[i] = ma_sound_init_from_file_w(&impl->engine, path.c_str(),
                                                    MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION,
                                                    nullptr, nullptr, &impl->effects[i]) == MA_SUCCESS;
    }
    // Audio owns its randomness: do not perturb gameplay/simulation RNG state.
    std::mt19937 random(static_cast<std::mt19937::result_type>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        random.seed(std::random_device{}());
    } catch (const std::exception &) {
        logLine("Audio randomness unavailable; using clock-seeded soundtrack selection.");
    }
    // next() advances before loading, and retains its missing-file fallback.
    impl->index = randomMusicStart(random) - 1;
    next();
}
Audio::~Audio() {
    if (!impl->ready)
        return;
    if (impl->musicReady)
        ma_sound_uninit(&impl->music);
    for (size_t i = 0; i < impl->loaded.size(); i++)
        if (impl->loaded[i])
            ma_sound_uninit(&impl->effects[i]);
    ma_engine_uninit(&impl->engine);
}
void Audio::next() {
    if (!impl->ready)
        return;
    if (impl->musicReady)
        ma_sound_uninit(&impl->music);
    impl->musicReady = false;
    for (int attempt = 0; attempt < musicTrackCount; attempt++) {
        impl->index = nextMusicIndex(impl->index);
        auto path = (impl->folder / ("music-" + std::to_string(impl->index) + ".mp3")).wstring();
        if (ma_sound_init_from_file_w(&impl->engine, path.c_str(),
                                      MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr,
                                      nullptr, &impl->music) == MA_SUCCESS) {
            impl->musicReady = true;
            impl->volume = 0;
            ma_sound_set_volume(&impl->music, 0);
            ma_sound_start(&impl->music);
            break;
        }
    }
}
void Audio::update(float dt, float musicVolume, float effectsVolume, bool focused) {
    if (!impl->ready)
        return;
    for (float &t : impl->cooldown)
        t = std::max(0.f, t - dt);
    impl->effectVolume = focused ? std::clamp(effectsVolume, 0.f, 1.f) : 0;
    // Mute immediately at zero; otherwise smoothly fade both focus changes and track starts.
    const float target = focused ? std::clamp(musicVolume, 0.f, 1.f) : 0;
    impl->volume = target == 0 ? 0 : impl->volume + (target - impl->volume) * (1 - std::exp(-dt * 3));
    if (impl->musicReady) {
        ma_sound_set_volume(&impl->music, impl->volume);
        if (ma_sound_at_end(&impl->music))
            next();
    }
    for (size_t i = 0; i < impl->loaded.size(); i++)
        if (impl->loaded[i])
            ma_sound_set_volume(&impl->effects[i], impl->effectVolume);
}
void Audio::play(Sound event) {
    auto i = size_t(event);
    if (!impl->ready || i >= impl->loaded.size() || !impl->loaded[i] || impl->cooldown[i] > 0)
        return;
    impl->cooldown[i] = event == Sound::Tick || event == Sound::Rotate ? .075f : .12f;
    ma_sound_stop(&impl->effects[i]);
    ma_sound_seek_to_pcm_frame(&impl->effects[i], 0);
    ma_sound_set_volume(&impl->effects[i], impl->effectVolume);
    ma_sound_start(&impl->effects[i]);
}
std::string Audio::track() const {
    const char *titles[] = {"AFTERIMAGE V2", "CUSTOM TRACK 02", "VELVET CIRCUIT V3", "CHROME HONEY",
                            "RESURGENCE LOOP"};
    return impl->musicReady ? titles[impl->index]
           : impl->ready    ? "Music not imported"
                            : "Audio unavailable / muted test";
}
bool Audio::available() const {
    return impl->ready;
}
void runAudioTests(const std::filesystem::path &folder) {
    // Deterministic coverage of random startup and subsequent sequential wrapping.
    std::mt19937 random(0x505450u);
    std::array<bool, musicTrackCount> seen{};
    for (int trial = 0; trial < 1024; ++trial) {
        const int start = randomMusicStart(random);
        if (start < 0 || start >= musicTrackCount)
            throw std::runtime_error("Random soundtrack index out of range");
        seen[start] = true;
        int current = start - 1;
        for (int step = 0; step <= musicTrackCount; ++step) {
            current = nextMusicIndex(current);
            if (current != (start + step) % musicTrackCount)
                throw std::runtime_error("Soundtrack startup/playlist wrap mismatch");
        }
    }
    if (!std::all_of(seen.begin(), seen.end(), [](bool selected) { return selected; }))
        throw std::runtime_error("Random soundtrack startup did not reach every track");
    logLine("Random soundtrack startup and sequential playlist tests passed.");
    std::vector<std::filesystem::path> files;
    for (auto name : {"tick", "rotate", "dock", "grab", "throw", "jump", "unlock", "click", "victory"})
        files.push_back(folder / (std::string(name) + ".wav"));
    for (int i = 0; i < musicTrackCount; i++) {
        auto path = folder / ("music-" + std::to_string(i) + ".mp3");
        if (std::filesystem::exists(path))
            files.push_back(path);
    }
    for (const auto &path : files) {
        ma_decoder decoder{};
        auto config = ma_decoder_config_init(ma_format_f32, 2, 48000);
        if (ma_decoder_init_file_w(path.c_str(), &config, &decoder) != MA_SUCCESS)
            throw std::runtime_error("Cannot decode game audio: " + path.string());
        ma_uint64 length = 0;
        ma_decoder_get_length_in_pcm_frames(&decoder, &length);
        const bool music = path.extension() == ".mp3";
        if (music)
            ma_decoder_seek_to_pcm_frame(&decoder, std::min<ma_uint64>(length / 2, 48000 * 30));
        float samples[8192]{};
        ma_uint64 read = 0;
        ma_decoder_read_pcm_frames(&decoder, samples, 4096, &read);
        ma_decoder_uninit(&decoder);
        float peak = 0;
        for (size_t i = 0; i < read * 2; i++) {
            if (!std::isfinite(samples[i]))
                throw std::runtime_error("Nonfinite audio sample");
            peak = std::max(peak, std::abs(samples[i]));
        }
        if (!read || peak < .00001f || peak > 1.f || (music && length < 48000))
            throw std::runtime_error("Silent, clipped or truncated audio asset: " + path.string());
    }
    logLine("Audio decode/seek tests passed: " + std::to_string(files.size()) + " assets.");
}
