#include "core/paths.hpp"

#include <cstdlib>
#include <system_error>

namespace fzgx {

namespace {

std::filesystem::path get_env_path(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, name) == 0 && value != nullptr) {
        std::filesystem::path p(value);
        std::free(value);
        return p;
    }
    return {};
#else
    if (const char* value = std::getenv(name); value != nullptr && value[0] != '\0') {
        return std::filesystem::path(value);
    }
    return {};
#endif
}

} // namespace

AppPaths resolve_app_paths() {
    AppPaths paths;
#if defined(_WIN32)
    auto appdata = get_env_path("APPDATA");
    if (appdata.empty()) {
        appdata = get_env_path("USERPROFILE");
    }
    if (appdata.empty()) {
        appdata = std::filesystem::current_path();
    }
    paths.config_dir = appdata / "fzero-gx-pc";
    paths.data_dir = appdata / "fzero-gx-pc";
#else
    auto xdg_config = get_env_path("XDG_CONFIG_HOME");
    auto xdg_data = get_env_path("XDG_DATA_HOME");
    auto home = get_env_path("HOME");
    if (home.empty()) {
        home = std::filesystem::current_path();
    }
    if (xdg_config.empty()) {
        xdg_config = home / ".config";
    }
    if (xdg_data.empty()) {
        xdg_data = home / ".local" / "share";
    }
    paths.config_dir = xdg_config / "fzero-gx-pc";
    paths.data_dir = xdg_data / "fzero-gx-pc";
#endif
    paths.config_file = paths.config_dir / "config.json";
    paths.card_slot_a_dir = paths.data_dir / "memory_card" / "slot_a";
    paths.card_slot_b_dir = paths.data_dir / "memory_card" / "slot_b";
    paths.shaders_dir = paths.data_dir / "shaders";
    return paths;
}

bool ensure_app_directories(const AppPaths& paths, std::string* error_out) {
    std::error_code ec;
    for (const auto& dir : {paths.config_dir, paths.data_dir, paths.card_slot_a_dir,
                            paths.card_slot_b_dir, paths.shaders_dir}) {
        if (!dir.empty()) {
            std::filesystem::create_directories(dir, ec);
            if (ec) {
                if (error_out) {
                    *error_out = "Failed to create directory '" + dir.string() + "': " + ec.message();
                }
                return false;
            }
        }
    }
    return true;
}

} // namespace fzgx
