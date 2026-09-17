#pragma once
#include "game.h"
namespace lab {
Level makePlayLevel(bool fluidRoom = false, bool boat = false, bool deepPool = false);
void runPlayTests();
// Receiver calibration is radiant flux in the 510–570 nm band, before the visual EMA.
constexpr float receiverThreshold = .10f;
constexpr XMFLOAT4 receiverTarget{6, 1.5f, 5.1f, .65f};
} // namespace lab
