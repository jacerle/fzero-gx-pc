#pragma once

#include <functional>

namespace fzgx {

class AudioEngine;
class DiscImage;
class GciCardManager;
class InputManager;

// Host services behind include/fzgx_port/platform_api.h (and thus the native Dolphin SDK).
void bind_platform_services(DiscImage* disc, InputManager* input, GciCardManager* card, AudioEngine* audio);

// Runs at every VIWaitForRetrace, before the NTSC (59.94 Hz) pacing sleep: the host frame
// (events, input, overlay, render) while game code owns the thread. Empty clears it.
void set_vi_retrace_hook(std::function<void()> hook);

} // namespace fzgx
