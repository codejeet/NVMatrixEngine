#pragma once
#include <filesystem>
#include <memory>
#include <string>
enum class Sound { Tick, Rotate, Dock, Grab, Throw, Jump, Unlock, Click, Victory, Count };
void runAudioTests(const std::filesystem::path &folder);
class Audio {
  public:
    Audio(const std::filesystem::path &folder, bool silent = false);
    ~Audio();
    void update(float dt, float musicVolume, float effectsVolume, bool focused);
    void play(Sound event);
    void next();
    std::string track() const;
    bool available() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
