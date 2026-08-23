// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/types.h"
#include "core/file_sys/game_backend.h"

#ifdef __APPLE__
#include <CoreFoundation/CFBundle.h>
#include <dlfcn.h>
#include <sys/param.h>
#endif

#ifndef MAX_PATH
#ifdef _WIN32
// This is the maximum number of UTF-16 code units permissible in Windows file paths
#define MAX_PATH 260
#include <Shlobj.h>
#include <windows.h>
#else
// This is the maximum number of UTF-8 code units permissible in all other OSes' file paths
#define MAX_PATH 1024
#endif
#endif

#include <QString>

namespace Common::FS {

namespace fs = std::filesystem;

#ifdef __APPLE__
using IsTranslocatedURLFunc = Boolean (*)(CFURLRef path, bool* isTranslocated,
                                          CFErrorRef* __nullable error);
using CreateOriginalPathForURLFunc = CFURLRef __nullable (*)(CFURLRef translocatedPath,
                                                             CFErrorRef* __nullable error);

static CFURLRef UntranslocateBundlePath(const CFURLRef bundle_path) {
    CFURLRef path = nullptr;
    if (void* security_handle =
            dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY)) {
        const auto IsTranslocatedURL = reinterpret_cast<IsTranslocatedURLFunc>(
            dlsym(security_handle, "SecTranslocateIsTranslocatedURL"));
        const auto CreateOriginalPathForURL = reinterpret_cast<CreateOriginalPathForURLFunc>(
            dlsym(security_handle, "SecTranslocateCreateOriginalPathForURL"));

        bool is_translocated = false;
        if (IsTranslocatedURL && CreateOriginalPathForURL &&
            IsTranslocatedURL(bundle_path, &is_translocated, nullptr) && is_translocated) {
            path = CreateOriginalPathForURL(bundle_path, nullptr);
        }

        dlclose(security_handle);
    }
    return path;
}

static std::optional<std::filesystem::path> GetBundleParentDirectory() {
    std::optional<std::filesystem::path> path = std::nullopt;
    if (CFBundleRef bundle_ref = CFBundleGetMainBundle()) {
        if (CFURLRef bundle_url_ref = CFBundleCopyBundleURL(bundle_ref)) {
            CFURLRef untranslocated_url_ref = UntranslocateBundlePath(bundle_url_ref);

            char app_bundle_path[MAXPATHLEN];
            if (CFURLGetFileSystemRepresentation(
                    untranslocated_url_ref ? untranslocated_url_ref : bundle_url_ref, true,
                    reinterpret_cast<u8*>(app_bundle_path), sizeof(app_bundle_path))) {
                std::filesystem::path bundle_path{app_bundle_path};
                path = bundle_path.parent_path();
            }

            if (untranslocated_url_ref) {
                CFRelease(untranslocated_url_ref);
            }
            CFRelease(bundle_url_ref);
        }
    }
    return path;
}
#endif

namespace {

std::unordered_map<PathType, fs::path> user_paths;
fs::path application_directory;
fs::path standard_user_directory;
bool standard_user_directory_existed{};
bool user_paths_initialized{};

std::pair<fs::path, fs::path> GetStandardDirectories() {
#ifdef __APPLE__
    const auto app_support = fs::path{getenv("HOME")} / "Library" / "Application Support";
    return {app_support / "shadPS4", app_support / "shadPS4QtLauncher"};
#elif defined(__linux__)
    const char* xdg_data_home = getenv("XDG_DATA_HOME");
    const auto data_home = xdg_data_home != nullptr && strlen(xdg_data_home) > 0
                               ? fs::path{xdg_data_home}
                               : fs::path{getenv("HOME")} / ".local" / "share";
    return {data_home / "shadPS4", data_home / "shadPS4QtLauncher"};
#elif _WIN32
    TCHAR appdata[MAX_PATH]{};
    if (SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, 0, appdata) != S_OK) {
        throw std::runtime_error("Unable to resolve the AppData directory");
    }
    const fs::path appdata_path{appdata};
    return {appdata_path / "shadPS4", appdata_path / "shadPS4QtLauncher"};
#endif
}

void CreateLauncherPaths(const fs::path& launcher_dir) {
    const auto create_path = [](PathType shad_path, const fs::path& new_path) {
        fs::create_directories(new_path);
        user_paths.insert_or_assign(shad_path, new_path);
    };

    create_path(PathType::LauncherDir, launcher_dir);
    create_path(PathType::LauncherMetaData, launcher_dir / METADATA_DIR);
    create_path(PathType::VersionDir, launcher_dir / VERSION_DIR);
}

void CreateTrophyNotice(const fs::path& user_dir) {
    const auto notice_path = user_dir / CUSTOM_TROPHY / "Notice.txt";
    if (fs::exists(notice_path)) {
        return;
    }

    std::ofstream notice_file(notice_path);
    if (notice_file.is_open()) {
        notice_file
            // clang-format off
<< "++++++++++++++++++++++++++++++++\n"
"+ Custom Trophy Images / Sound +\n"
"++++++++++++++++++++++++++++++++\n\n"

"You can add custom images to the trophies.\n"
"*We recommend a square resolution image, for example 200x200, 500x500, the same size as the height and width.\n"
"In this folder ('user\\custom_trophy'), add the files with the following names:\n\n"
"bronze.png\n"
"silver.png\n"
"gold.png\n"
"platinum.png\n\n"

"You can add a custom sound for trophy notifications.\n"
"*By default, no audio is played unless it is in this folder and you are using the QT version.\n"
"In this folder ('user\\custom_trophy'), add the files with the following names:\n\n"

"trophy.wav OR trophy.mp3";
        // clang-format on
        notice_file.close();
    }
}

} // namespace

void InitializeUserPaths(const fs::path& app_dir) {
    if (user_paths_initialized) {
        return;
    }
    if (app_dir.empty()) {
        throw std::runtime_error("The launcher application directory is empty");
    }

    application_directory = fs::absolute(app_dir).lexically_normal();
#if defined(__APPLE__)
    if (const auto bundle_dir = GetBundleParentDirectory()) {
        application_directory = *bundle_dir;
        fs::current_path(*bundle_dir);
    }
#endif

    if (!fs::is_directory(application_directory)) {
        throw std::runtime_error("The launcher application directory is invalid");
    }

    auto [platform_user_directory, platform_launcher_directory] = GetStandardDirectories();
    standard_user_directory = std::move(platform_user_directory);

    std::error_code exists_error;
    standard_user_directory_existed = fs::exists(standard_user_directory, exists_error);
    if (exists_error) {
        // Treat an unreadable path as pre-existing so cleanup can never happen automatically.
        standard_user_directory_existed = true;
    }

    const auto portable_user_directory = GetPortableUserDirectory();
    const auto portable_launcher_directory = application_directory / PORTABLE_LAUNCHER_DIR;
    SetUserDirectory(fs::is_directory(portable_user_directory) ? portable_user_directory
                                                               : standard_user_directory);
    CreateLauncherPaths(fs::is_directory(portable_launcher_directory)
                            ? portable_launcher_directory
                            : platform_launcher_directory);
    user_paths_initialized = true;
}

const fs::path& GetApplicationDirectory() {
    if (application_directory.empty()) {
        throw std::logic_error("User paths have not been initialized");
    }
    return application_directory;
}

const fs::path& GetStandardUserDirectory() {
    if (standard_user_directory.empty()) {
        throw std::logic_error("User paths have not been initialized");
    }
    return standard_user_directory;
}

bool StandardUserDirectoryExistedAtStartup() {
    return standard_user_directory_existed;
}

fs::path GetPortableUserDirectory() {
    return GetApplicationDirectory() / PORTABLE_DIR;
}

bool IsPortableUserDirectory() {
    return user_paths_initialized && GetUserPath(PathType::UserDir) == GetPortableUserDirectory();
}

void SetUserDirectory(const fs::path& user_dir) {
    fs::create_directories(user_dir);
    const auto create_path = [](PathType shad_path, const fs::path& new_path) {
        fs::create_directories(new_path);
        user_paths.insert_or_assign(shad_path, new_path);
    };

    create_path(PathType::UserDir, user_dir);
    create_path(PathType::LogDir, user_dir / LOG_DIR);
    create_path(PathType::ScreenshotsDir, user_dir / SCREENSHOTS_DIR);
    create_path(PathType::ShaderDir, user_dir / SHADER_DIR);
    create_path(PathType::GameDataDir, user_dir / GAMEDATA_DIR);
    create_path(PathType::TempDataDir, user_dir / TEMPDATA_DIR);
    create_path(PathType::SysModuleDir, user_dir / SYSMODULES_DIR);
    create_path(PathType::DownloadDir, user_dir / DOWNLOAD_DIR);
    create_path(PathType::CapturesDir, user_dir / CAPTURES_DIR);
    create_path(PathType::CheatsDir, user_dir / CHEATS_DIR);
    create_path(PathType::PatchesDir, user_dir / PATCHES_DIR);
    create_path(PathType::MetaDataDir, user_dir / METADATA_DIR);
    create_path(PathType::CustomTrophy, user_dir / CUSTOM_TROPHY);
    create_path(PathType::CustomConfigs, user_dir / CUSTOM_CONFIGS);
    create_path(PathType::CacheDir, user_dir / CACHE_DIR);
    create_path(PathType::FontsDir, user_dir / FONTS_DIR);
    create_path(PathType::HomeDir, user_dir / HOME_DIR);
    create_path(PathType::TrophyDir, user_dir / TROPHY_DIR);
    CreateTrophyNotice(user_dir);
}

bool ValidatePath(const fs::path& path) {
    if (path.empty()) {
        LOG_ERROR(Common_Filesystem, "Input path is empty, path={}", PathToUTF8String(path));
        return false;
    }

#ifdef _WIN32
    if (path.u16string().size() >= MAX_PATH) {
        LOG_ERROR(Common_Filesystem, "Input path is too long, path={}", PathToUTF8String(path));
        return false;
    }
#else
    if (path.u8string().size() >= MAX_PATH) {
        LOG_ERROR(Common_Filesystem, "Input path is too long, path={}", PathToUTF8String(path));
        return false;
    }
#endif

    return true;
}

std::string PathToUTF8String(const std::filesystem::path& path) {
    const auto u8_string = path.u8string();
    return std::string{u8_string.begin(), u8_string.end()};
}

const fs::path& GetUserPath(PathType shad_path) {
    return user_paths.at(shad_path);
}

std::string GetUserPathString(PathType shad_path) {
    return PathToUTF8String(GetUserPath(shad_path));
}

void SetUserPath(PathType shad_path, const fs::path& new_path) {
    if (!std::filesystem::is_directory(new_path)) {
        LOG_ERROR(Common_Filesystem, "Filesystem object at new_path={} is not a directory",
                  PathToUTF8String(new_path));
        return;
    }

    user_paths.insert_or_assign(shad_path, new_path);
}

std::optional<fs::path> FindGameByID(const fs::path& dir, const std::string& game_id,
                                     int max_depth) {
    if (max_depth < 0) {
        return std::nullopt;
    }

    // Check if this is the game we're looking for
    if (dir.filename() == game_id && fs::exists(dir / "sce_sys" / "param.sfo")) {
        auto eboot_path = dir / "eboot.bin";
        if (fs::exists(eboot_path)) {
            return eboot_path;
        }
    }

    if (const auto zar_candidate = dir / (game_id + ".zar");
        Core::FileSys::IsZArchiveFile(zar_candidate)) {
        if (Core::FileSys::ReadGameFile(zar_candidate, "sce_sys/param.sfo").has_value()) {
            return zar_candidate / "eboot.bin";
        }
    }

    // Recursively search subdirectories
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        if (auto found = FindGameByID(entry.path(), game_id, max_depth - 1)) {
            return found;
        }
    }

    return std::nullopt;
}

void PathToQString(QString& result, const std::filesystem::path& path) {
#ifdef _WIN32
    result = QString::fromStdWString(path.wstring());
#else
    result = QString::fromStdString(path.string());
#endif
}

std::filesystem::path PathFromQString(const QString& path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}

} // namespace Common::FS
