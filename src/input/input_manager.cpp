#include "input/input_manager.hpp"

#include <algorithm>
#include <cmath>

namespace fzgx {

namespace {

constexpr size_t kNumGcButtons = 12;

constexpr uint16_t kGcBits[kNumGcButtons] = {
    kGcBtnA, kGcBtnB, kGcBtnX, kGcBtnY, kGcBtnZ, kGcBtnStart,
    kGcBtnL, kGcBtnR, kGcBtnUp, kGcBtnDown, kGcBtnLeft, kGcBtnRight,
};

constexpr const char* kGcNames[kNumGcButtons] = {
    "A", "B", "X", "Y", "Z", "Start",
    "L (Digital)", "R (Digital)", "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
};

constexpr SDL_GamepadButton kDefaultSdlMap[kNumGcButtons] = {
    SDL_GAMEPAD_BUTTON_SOUTH,        // A
    SDL_GAMEPAD_BUTTON_WEST,         // B
    SDL_GAMEPAD_BUTTON_EAST,         // X
    SDL_GAMEPAD_BUTTON_NORTH,        // Y
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, // Z
    SDL_GAMEPAD_BUTTON_START,        // Start
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,// L digital fallback
    SDL_GAMEPAD_BUTTON_INVALID,      // R digital comes primarily from trigger threshold
    SDL_GAMEPAD_BUTTON_DPAD_UP,
    SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
};

} // namespace

const char* gc_button_name(size_t index) {
    return index < kNumGcButtons ? kGcNames[index] : "Unknown";
}

uint16_t gc_button_bit(size_t index) {
    return index < kNumGcButtons ? kGcBits[index] : 0;
}

void shape_stick(float raw_x,
                 float raw_y,
                 float deadzone,
                 float outer_gate,
                 float& out_norm_x,
                 float& out_norm_y,
                 int8_t& out_gc_x,
                 int8_t& out_gc_y) {
    const float mag = std::sqrt(raw_x * raw_x + raw_y * raw_y);
    const float dz = std::clamp(deadzone, 0.0f, 0.9f);
    const float gate = std::clamp(outer_gate, dz + 0.05f, 1.5f);

    if (mag <= dz || mag < 1e-6f) {
        out_norm_x = 0.0f;
        out_norm_y = 0.0f;
        out_gc_x = 0;
        out_gc_y = 0;
        return;
    }

    const float scaled = std::clamp((mag - dz) / (gate - dz), 0.0f, 1.0f);
    out_norm_x = (raw_x / mag) * scaled;
    out_norm_y = (raw_y / mag) * scaled;

    const float gx = std::clamp(out_norm_x * 127.0f, -128.0f, 127.0f);
    const float gy = std::clamp(out_norm_y * 127.0f, -128.0f, 127.0f);
    out_gc_x = static_cast<int8_t>(std::lround(gx));
    out_gc_y = static_cast<int8_t>(std::lround(gy));
}

void shape_trigger(float raw_val,
                   float analog_deadzone,
                   float digital_threshold,
                   float& out_norm,
                   uint8_t& out_gc_analog,
                   bool& out_digital_click) {
    const float clamped = std::clamp(raw_val, 0.0f, 1.0f);
    const float dz = std::clamp(analog_deadzone, 0.0f, 0.9f);
    if (clamped <= dz) {
        out_norm = 0.0f;
        out_gc_analog = 0;
        out_digital_click = false;
        return;
    }
    out_norm = std::clamp((clamped - dz) / (1.0f - dz), 0.0f, 1.0f);
    out_gc_analog = static_cast<uint8_t>(std::lround(out_norm * 255.0f));
    out_digital_click = (clamped >= std::clamp(digital_threshold, 0.05f, 1.0f));
}

SdlInputProvider::~SdlInputProvider() {
    for (auto& pad : open_pads_) {
        if (pad.handle) {
            SDL_CloseGamepad(pad.handle);
        }
    }
    open_pads_.clear();
}

void SdlInputProvider::refresh_devices() {
    for (auto& pad : open_pads_) {
        if (pad.handle) {
            SDL_CloseGamepad(pad.handle);
        }
    }
    open_pads_.clear();

    if (!SDL_WasInit(SDL_INIT_GAMEPAD)) {
        return;
    }

    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        if (SDL_Gamepad* gp = SDL_OpenGamepad(ids[i])) {
            const char* n = SDL_GetGamepadName(gp);
            open_pads_.push_back({ids[i], gp, n ? n : "SDL Gamepad"});
        }
    }
    SDL_free(ids);
}

void SdlInputProvider::handle_sdl_event(const SDL_Event& ev) {
    if (ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
        refresh_devices();
    }
}

std::vector<std::string> SdlInputProvider::connected_device_names() const {
    std::vector<std::string> names;
    names.reserve(open_pads_.size());
    for (const auto& p : open_pads_) {
        names.push_back(p.name);
    }
    return names;
}

bool SdlInputProvider::overlay_chord_triggered() {
    bool held = false;
    for (const auto& pad : open_pads_) {
        if (!pad.handle) {
            continue;
        }
        const bool guide = SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_GUIDE);
        const bool back_start =
            SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_BACK) &&
            SDL_GetGamepadButton(pad.handle, SDL_GAMEPAD_BUTTON_START);
        if (guide || back_start) {
            held = true;
            break;
        }
    }
    const bool edge = held && !prev_chord_held_;
    prev_chord_held_ = held;
    return edge;
}

void SdlInputProvider::poll(const AppConfig& config, std::array<PadState, 4>& ports) {
    for (size_t i = 0; i < 4; ++i) {
        ports[i] = {};
        if (i >= config.controllers.size() || !config.controllers[i].enabled) {
            continue;
        }
        const auto& pc = config.controllers[i];

        SDL_Gamepad* gp = nullptr;
        std::string label;
        if (pc.device_name != "Auto" && pc.device_name != "Keyboard") {
            for (const auto& candidate : open_pads_) {
                if (candidate.name == pc.device_name) {
                    gp = candidate.handle;
                    label = candidate.name;
                    break;
                }
            }
        }
        if (!gp && pc.device_name != "Keyboard" && i < open_pads_.size()) {
            gp = open_pads_[i].handle;
            label = open_pads_[i].name;
        }

        if (gp) {
            ports[i].connected = true;
            ports[i].device_label = label;

            for (size_t b = 0; b < kNumGcButtons; ++b) {
                SDL_GamepadButton btn = kDefaultSdlMap[b];
                if (b < pc.button_map.size() && pc.button_map[b] >= 0) {
                    btn = static_cast<SDL_GamepadButton>(pc.button_map[b]);
                }
                if (btn != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(gp, btn)) {
                    ports[i].buttons |= kGcBits[b];
                }
            }

            auto norm_axis = [](Sint16 v) -> float {
                return v < 0 ? static_cast<float>(v) / 32768.0f : static_cast<float>(v) / 32767.0f;
            };
            const float lx = norm_axis(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX));
            // GameCube Y+ is UP; SDL Y+ is DOWN.
            const float ly = -norm_axis(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY));
            shape_stick(lx, ly, pc.stick_deadzone, pc.stick_outer_gate,
                        ports[i].norm_stick_x, ports[i].norm_stick_y,
                        ports[i].stick_x, ports[i].stick_y);

            const float rx = norm_axis(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX));
            const float ry = -norm_axis(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY));
            shape_stick(rx, ry, pc.cstick_deadzone, pc.stick_outer_gate,
                        ports[i].norm_substick_x, ports[i].norm_substick_y,
                        ports[i].substick_x, ports[i].substick_y);

            const float lt = static_cast<float>(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)) / 32767.0f;
            const float rt = static_cast<float>(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) / 32767.0f;

            bool l_click = false;
            bool r_click = false;
            shape_trigger(lt, pc.trigger_analog_deadzone, pc.trigger_digital_threshold,
                          ports[i].norm_trigger_l, ports[i].trigger_l, l_click);
            shape_trigger(rt, pc.trigger_analog_deadzone, pc.trigger_digital_threshold,
                          ports[i].norm_trigger_r, ports[i].trigger_r, r_click);
            if (l_click) {
                ports[i].buttons |= kGcBtnL;
            }
            if (r_click) {
                ports[i].buttons |= kGcBtnR;
            }
        }

        // Keyboard fallback on Port 1 when no gamepad is active or explicitly selected.
        if (i == 0 && !ports[0].connected && SDL_WasInit(SDL_INIT_VIDEO)) {
            ports[0].connected = true;
            ports[0].device_label = "Keyboard (WASD/Arrows + Z/X/Q/E)";
            const bool* keys = SDL_GetKeyboardState(nullptr);
            if (keys) {
                float kx = 0.0f;
                float ky = 0.0f;
                if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) kx -= 1.0f;
                if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) kx += 1.0f;
                if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) ky += 1.0f;
                if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) ky -= 1.0f;
                shape_stick(kx, ky, 0.05f, 1.0f,
                            ports[0].norm_stick_x, ports[0].norm_stick_y,
                            ports[0].stick_x, ports[0].stick_y);

                if (keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_Z]) ports[0].buttons |= kGcBtnA;
                if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_X]) ports[0].buttons |= kGcBtnB;
                if (keys[SDL_SCANCODE_C]) ports[0].buttons |= kGcBtnX;
                if (keys[SDL_SCANCODE_V]) ports[0].buttons |= kGcBtnY;
                if (keys[SDL_SCANCODE_RETURN]) ports[0].buttons |= kGcBtnStart;
                if (keys[SDL_SCANCODE_Q]) {
                    ports[0].trigger_l = 255;
                    ports[0].norm_trigger_l = 1.0f;
                    ports[0].buttons |= kGcBtnL;
                }
                if (keys[SDL_SCANCODE_E]) {
                    ports[0].trigger_r = 255;
                    ports[0].norm_trigger_r = 1.0f;
                    ports[0].buttons |= kGcBtnR;
                }
            }
        }
    }
}

void SdlInputProvider::set_rumble(int port, float low_freq, float high_freq, uint32_t duration_ms) {
    if (port < 0 || static_cast<size_t>(port) >= open_pads_.size()) {
        return;
    }
    if (open_pads_[static_cast<size_t>(port)].handle) {
        const auto lo = static_cast<Uint16>(std::clamp(low_freq, 0.0f, 1.0f) * 65535.0f);
        const auto hi = static_cast<Uint16>(std::clamp(high_freq, 0.0f, 1.0f) * 65535.0f);
        SDL_RumbleGamepad(open_pads_[static_cast<size_t>(port)].handle, lo, hi, duration_ms);
    }
}

InputManager::InputManager()
    : sdl_provider_(std::make_unique<SdlInputProvider>()),
      gc_adapter_provider_(std::make_unique<GcUsbAdapterProvider>()) {}

void InputManager::handle_sdl_event(const SDL_Event& ev) {
    sdl_provider_->handle_sdl_event(ev);
}

void InputManager::refresh_devices() {
    sdl_provider_->refresh_devices();
}

void InputManager::poll(const AppConfig& config) {
    sdl_provider_->poll(config, ports_);
    if (gc_adapter_provider_->is_adapter_detected()) {
        gc_adapter_provider_->poll(config, ports_);
    }
}

void InputManager::test_rumble(int port) {
    sdl_provider_->set_rumble(port, 0.5f, 0.75f, 250);
}

void InputManager::set_rumble(int port, float low_freq, float high_freq, uint32_t duration_ms) {
    sdl_provider_->set_rumble(port, low_freq, high_freq, duration_ms);
}

const PadState& InputManager::port_state(int port) const {
    if (port < 0 || port >= 4) {
        return ports_[0];
    }
    return ports_[static_cast<size_t>(port)];
}

std::vector<std::string> InputManager::connected_device_names() const {
    return sdl_provider_->connected_device_names();
}

bool InputManager::poll_overlay_gamepad_toggle() {
    return sdl_provider_->overlay_chord_triggered();
}

} // namespace fzgx
