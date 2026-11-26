#pragma once

#include <filesystem>

namespace fzgx {

struct AppPaths {
    std::filesystem::path config_dir;
    std::filesystem::path data_dir;
    std::filesystem::path config_file;
    std::filesystem::path card_slot_a_dir;
    std::filesystem::path card_slot_b_dir;
    std::filesystem::path shaders_dir;
};

AppPaths resolve_app_paths();
bool ensure_app_directories(const AppPaths& paths, std::string* error_out = nullptr);

} // namespace fzgx
