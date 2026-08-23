// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/path_util.h"
#include "common/portable_user.h"
#include "common/sha1.h"

namespace Common::FS {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::string_view StagingDirectory = ".shadps4-user-migration";
constexpr std::string_view StagingMarker = ".shadps4-portable-staging";
constexpr std::string_view ManifestFile = ".shadps4-portable-manifest.json";
constexpr std::string_view CleanupAskFile = ".shadps4-portable-cleanup.ask";
constexpr std::string_view CleanupAutomaticFile = ".shadps4-portable-cleanup.auto";
constexpr std::string_view CleanupCompleteFile = ".shadps4-portable-cleanup.done";
constexpr std::string_view WriteProbeFile = ".shadps4-portable-write-probe";
constexpr std::string_view MarkerContents = "shadPS4 portable migration v1\n";

struct ManifestEntry {
    std::string path;
    bool is_directory{};
    std::uintmax_t size{};
    std::string sha1;

    bool operator==(const ManifestEntry&) const = default;
};

bool Fail(std::string& error, std::string message) {
    error = std::move(message);
    return false;
}

std::string RelativePathString(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {value.begin(), value.end()};
}

fs::path RelativePath(const std::string& value) {
    return fs::path{std::u8string{value.begin(), value.end()}};
}

bool HashFile(const fs::path& path, std::string& result, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Fail(error, "Could not open " + PathToUTF8String(path) + " for verification.");
    }

    sha1::SHA1 hasher;
    std::array<char, 64 * 1024> buffer;
    while (file) {
        file.read(buffer.data(), buffer.size());
        const auto bytes_read = file.gcount();
        if (bytes_read > 0) {
            hasher.processBytes(buffer.data(), static_cast<std::size_t>(bytes_read));
        }
    }
    if (!file.eof()) {
        return Fail(error, "Could not read " + PathToUTF8String(path) + " for verification.");
    }

    sha1::SHA1::digest8_t digest;
    hasher.getDigestBytes(digest);
    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        hex << std::setw(2) << static_cast<unsigned>(byte);
    }
    result = hex.str();
    return true;
}

bool BuildManifest(const fs::path& root, std::vector<ManifestEntry>& manifest,
                   std::string& error) {
    manifest.clear();

    std::error_code ec;
    if (!fs::exists(root, ec)) {
        return ec ? Fail(error, "Could not inspect " + PathToUTF8String(root) + ": " +
                                    ec.message())
                  : true;
    }
    if (fs::symlink_status(root, ec).type() != fs::file_type::directory || ec) {
        return Fail(error, "The migration source is not a regular directory: " +
                               PathToUTF8String(root));
    }

    fs::recursive_directory_iterator iterator(root, fs::directory_options::none, ec);
    const fs::recursive_directory_iterator end;
    while (iterator != end) {
        if (ec) {
            return Fail(error, "Could not enumerate " + PathToUTF8String(root) + ": " +
                                   ec.message());
        }

        const auto relative = iterator->path().lexically_relative(root);
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
            return Fail(error, "Unsafe path found while copying " +
                                   PathToUTF8String(iterator->path()));
        }

        const auto status = iterator->symlink_status(ec);
        if (ec) {
            return Fail(error, "Could not inspect " + PathToUTF8String(iterator->path()) + ": " +
                                   ec.message());
        }

        ManifestEntry item{.path = RelativePathString(relative)};
        if (status.type() == fs::file_type::directory) {
            item.is_directory = true;
        } else if (status.type() == fs::file_type::regular) {
            item.size = iterator->file_size(ec);
            if (ec) {
                return Fail(error, "Could not read the size of " +
                                       PathToUTF8String(iterator->path()) + ": " + ec.message());
            }
            if (!HashFile(iterator->path(), item.sha1, error)) {
                return false;
            }
        } else {
            return Fail(error, "Portable migration does not follow links or special files: " +
                                   PathToUTF8String(iterator->path()));
        }
        manifest.push_back(std::move(item));
        iterator.increment(ec);
    }
    if (ec) {
        return Fail(error,
                    "Could not enumerate " + PathToUTF8String(root) + ": " + ec.message());
    }

    std::ranges::sort(manifest, {}, &ManifestEntry::path);
    return true;
}

bool VerifyFile(const fs::path& path, const ManifestEntry& expected, std::string& error) {
    std::error_code ec;
    if (fs::symlink_status(path, ec).type() != fs::file_type::regular || ec) {
        return Fail(error, "The copied file is missing or invalid: " + PathToUTF8String(path));
    }
    if (fs::file_size(path, ec) != expected.size || ec) {
        return Fail(error, "The copied file has the wrong size: " + PathToUTF8String(path));
    }

    std::string hash;
    return HashFile(path, hash, error) &&
           (hash == expected.sha1 ||
            Fail(error, "The copied file failed verification: " + PathToUTF8String(path)));
}

bool CopyManifest(const fs::path& source, const fs::path& destination,
                  const std::vector<ManifestEntry>& manifest, std::string& error) {
    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec) {
        return Fail(error, "Could not create " + PathToUTF8String(destination) + ": " +
                               ec.message());
    }

    for (const auto& item : manifest) {
        const auto relative = RelativePath(item.path);
        const auto from = source / relative;
        const auto to = destination / relative;
        if (item.is_directory) {
            fs::create_directories(to, ec);
            if (ec) {
                return Fail(error,
                            "Could not create " + PathToUTF8String(to) + ": " + ec.message());
            }
            continue;
        }

        fs::create_directories(to.parent_path(), ec);
        if (ec) {
            return Fail(error, "Could not create " + PathToUTF8String(to.parent_path()) + ": " +
                                   ec.message());
        }

        if (fs::exists(to, ec)) {
            if (ec || !VerifyFile(to, item, error)) {
                if (!ec && error.empty()) {
                    error = "Portable destination conflicts with " + PathToUTF8String(to);
                }
                return false;
            }
        } else {
            if (ec || !fs::copy_file(from, to, fs::copy_options::none, ec) || ec) {
                return Fail(error, "Could not copy " + PathToUTF8String(from) + " to " +
                                       PathToUTF8String(to) + ": " + ec.message());
            }
            if (!VerifyFile(to, item, error)) {
                return false;
            }
        }
    }
    return true;
}

bool VerifyDestinationStructure(const fs::path& destination,
                                const std::vector<ManifestEntry>& manifest, std::string& error) {
    std::error_code ec;
    for (const auto& item : manifest) {
        const auto path = destination / RelativePath(item.path);
        const auto type = fs::symlink_status(path, ec).type();
        if (ec || type != (item.is_directory ? fs::file_type::directory
                                             : fs::file_type::regular)) {
            return Fail(error, "Portable data is missing: " + PathToUTF8String(path));
        }
    }
    return true;
}

bool WriteTextFile(const fs::path& path, std::string_view contents, std::string& error) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return Fail(error, "Could not write migration state to " + PathToUTF8String(path));
    }
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    file.flush();
    if (!file) {
        return Fail(error, "Could not persist migration state to " + PathToUTF8String(path));
    }
    return true;
}

bool WriteManifest(const fs::path& root, const std::vector<ManifestEntry>& manifest,
                   std::string& error) {
    json data{{"version", 1}, {"files", json::array()}};
    for (const auto& item : manifest) {
        data["files"].push_back({{"path", item.path},
                                 {"directory", item.is_directory},
                                 {"size", item.size},
                                 {"sha1", item.sha1}});
    }
    return WriteTextFile(root / ManifestFile, data.dump(2), error);
}

bool ReadManifest(const fs::path& root, std::vector<ManifestEntry>& manifest, std::string& error) {
    const auto path = root / ManifestFile;
    try {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return Fail(error, "The verified migration manifest is missing: " +
                                   PathToUTF8String(path));
        }

        json data;
        file >> data;
        if (data.at("version").get<int>() != 1 || !data.at("files").is_array()) {
            return Fail(error, "The portable migration manifest is invalid.");
        }

        manifest.clear();
        for (const auto& value : data.at("files")) {
            manifest.push_back({.path = value.at("path").get<std::string>(),
                                .is_directory = value.at("directory").get<bool>(),
                                .size = value.at("size").get<std::uintmax_t>(),
                                .sha1 = value.at("sha1").get<std::string>()});
        }
        std::ranges::sort(manifest, {}, &ManifestEntry::path);
        return true;
    } catch (const std::exception& exception) {
        return Fail(error, "Could not read the portable migration manifest: " +
                               std::string{exception.what()});
    }
}

bool HasMigrationState(const fs::path& root) {
    return fs::exists(root / CleanupAskFile) || fs::exists(root / CleanupAutomaticFile) ||
           fs::exists(root / CleanupCompleteFile);
}

bool WritePendingCleanup(const fs::path& root, std::string& error) {
    if (HasMigrationState(root)) {
        return true;
    }
    const auto marker = StandardUserDirectoryExistedAtStartup() ? CleanupAskFile
                                                                : CleanupAutomaticFile;
    return WriteTextFile(root / marker, MarkerContents, error);
}

bool VerifyPortableDirectoryIsWritable(const fs::path& root, std::string& error) {
    const auto probe = root / WriteProbeFile;
    std::error_code ec;
    if (fs::exists(probe, ec) || ec) {
        return Fail(error, "Could not safely test write access to the portable directory.");
    }
    if (!WriteTextFile(probe, MarkerContents, error)) {
        return false;
    }
    if (!fs::remove(probe, ec) || ec) {
        return Fail(error, "Could not clean up the portable directory write test" +
                               (ec ? ": " + ec.message() : "."));
    }
    return true;
}

bool IsDirectChildDirectory(const fs::path& path, const fs::path& parent,
                            std::string_view expected_name, std::string& error) {
    std::error_code ec;
    if (fs::symlink_status(path, ec).type() != fs::file_type::directory || ec) {
        return Fail(error, "The migration path is not a regular directory: " +
                               PathToUTF8String(path));
    }

    const auto canonical_path = fs::weakly_canonical(path, ec);
    if (ec) {
        return Fail(error, "Could not resolve " + PathToUTF8String(path) + ": " + ec.message());
    }
    const auto canonical_parent = fs::weakly_canonical(parent, ec);
    if (ec || canonical_path.parent_path() != canonical_parent ||
        RelativePathString(canonical_path.filename()) != expected_name) {
        return Fail(error, "Refusing to use a redirected migration directory: " +
                               PathToUTF8String(path));
    }
    return true;
}

bool HasStagingMarker(const fs::path& staging) {
    std::ifstream marker(staging / StagingMarker, std::ios::binary);
    return marker &&
           std::string{std::istreambuf_iterator<char>{marker}, std::istreambuf_iterator<char>{}} ==
               MarkerContents;
}

bool PrepareStagingDirectory(const fs::path& staging, std::string& error) {
    const auto expected = GetApplicationDirectory() / StagingDirectory;
    if (staging != expected) {
        return Fail(error, "Refusing to use an unexpected migration staging directory.");
    }

    std::error_code ec;
    if (fs::exists(staging, ec)) {
        if (ec || !IsDirectChildDirectory(staging, GetApplicationDirectory(), StagingDirectory,
                                          error) ||
            !HasStagingMarker(staging)) {
            return Fail(error, "The portable migration staging path already exists and was not "
                               "created by this launcher: " +
                                   PathToUTF8String(staging));
        }
        fs::remove_all(staging, ec);
        if (ec) {
            return Fail(error, "Could not clear the previous migration staging directory: " +
                                   ec.message());
        }
    }

    fs::create_directory(staging, ec);
    if (ec || !WriteTextFile(staging / StagingMarker, MarkerContents, error)) {
        return ec ? Fail(error, "Could not create the migration staging directory: " +
                                    ec.message())
                  : false;
    }
    return true;
}

bool IsSafeStandardUserDirectory(const fs::path& source, std::string& error) {
    if (!source.is_absolute() || source.filename() != "shadPS4" ||
        source == GetPortableUserDirectory() || source == source.root_path()) {
        return Fail(error, "Refusing to remove an unexpected shadPS4 directory: " +
                               PathToUTF8String(source));
    }

    return IsDirectChildDirectory(source, source.parent_path(), "shadPS4", error);
}

} // namespace

bool MigrateToPortableUserDirectory(std::string& error) {
    try {
        const auto source = GetStandardUserDirectory();
        const auto destination = GetPortableUserDirectory();
        if (source == destination) {
            return Fail(error, "The standard and portable shadPS4 directories are identical.");
        }

        std::error_code ec;
        const bool source_exists = fs::exists(source, ec);
        if (ec || (source_exists && !IsSafeStandardUserDirectory(source, error))) {
            return ec ? Fail(error, "Could not inspect the AppData shadPS4 directory: " +
                                        ec.message())
                      : false;
        }

        std::vector<ManifestEntry> manifest;
        if (!BuildManifest(source, manifest, error)) {
            return false;
        }

        const bool destination_exists = fs::exists(destination, ec);
        if (ec) {
            return Fail(error, "Could not inspect the portable directory: " + ec.message());
        }

        const auto copy_destination = destination_exists
                                          ? destination
                                          : GetApplicationDirectory() / StagingDirectory;
        if (!destination_exists && !PrepareStagingDirectory(copy_destination, error)) {
            return false;
        }
        if (destination_exists &&
            !IsDirectChildDirectory(destination, GetApplicationDirectory(), PORTABLE_DIR, error)) {
            return false;
        }

        if (!CopyManifest(source, copy_destination, manifest, error)) {
            return false;
        }

        std::vector<ManifestEntry> source_after_copy;
        if (!BuildManifest(source, source_after_copy, error) || source_after_copy != manifest) {
            return error.empty()
                       ? Fail(error, "The AppData shadPS4 directory changed during the copy. No "
                                     "files were deleted; run the migration again.")
                       : false;
        }

        if (!HasMigrationState(copy_destination) &&
            (!WriteManifest(copy_destination, manifest, error) ||
             !WritePendingCleanup(copy_destination, error))) {
            return false;
        }

        if (!destination_exists) {
            fs::rename(copy_destination, destination, ec);
            if (ec) {
                return Fail(error,
                            "Could not activate the portable user directory: " + ec.message());
            }
            // This marker is only needed to make an interrupted staging copy safely recoverable.
            fs::remove(destination / StagingMarker, ec);
        }

        SetUserDirectory(destination);
        return true;
    } catch (const std::exception& exception) {
        return Fail(error, "Portable migration failed: " + std::string{exception.what()});
    }
}

PortableCleanupMode GetPortableCleanupMode() {
    const auto root = GetPortableUserDirectory();
    std::error_code ec;
    if (fs::exists(root / CleanupCompleteFile, ec)) {
        return PortableCleanupMode::None;
    }
    if (ec) {
        return PortableCleanupMode::None;
    }
    if (fs::exists(root / CleanupAskFile, ec)) {
        return PortableCleanupMode::Ask;
    }
    if (ec) {
        return PortableCleanupMode::None;
    }
    if (fs::exists(root / CleanupAutomaticFile, ec)) {
        return PortableCleanupMode::Automatic;
    }
    return PortableCleanupMode::None;
}

bool CanRemoveStandardUserDirectory(std::string& error) {
    const auto source = GetStandardUserDirectory();
    std::error_code ec;
    if (!fs::exists(source, ec)) {
        return ec ? Fail(error, "Could not inspect the AppData shadPS4 directory: " + ec.message())
                  : true;
    }
    if (!IsSafeStandardUserDirectory(source, error)) {
        return false;
    }

    std::vector<ManifestEntry> expected;
    std::vector<ManifestEntry> current;
    if (!ReadManifest(GetPortableUserDirectory(), expected, error) ||
        !BuildManifest(source, current, error)) {
        return false;
    }
    if (current != expected) {
        return Fail(error, "The AppData shadPS4 directory changed after it was copied. It was kept "
                           "to prevent data loss.");
    }
    return VerifyDestinationStructure(GetPortableUserDirectory(), expected, error) &&
           VerifyPortableDirectoryIsWritable(GetPortableUserDirectory(), error);
}

bool CompletePortableCleanup(std::string& error) {
    const auto marker = GetPortableUserDirectory() / CleanupCompleteFile;
    std::error_code ec;
    if (fs::exists(marker, ec)) {
        return true;
    }
    if (ec) {
        return Fail(error, "Could not inspect the portable cleanup state: " + ec.message());
    }
    return WriteTextFile(marker, MarkerContents, error);
}

bool RemoveStandardUserDirectory(std::string& error) {
    if (!CanRemoveStandardUserDirectory(error)) {
        return false;
    }

    const auto source = GetStandardUserDirectory();
    std::error_code ec;
    if (!fs::exists(source, ec)) {
        return !ec || Fail(error, "Could not inspect the AppData shadPS4 directory: " +
                                     ec.message());
    }
    fs::remove_all(source, ec);
    std::error_code exists_error;
    const bool source_remains = fs::exists(source, exists_error);
    if (ec || exists_error || source_remains) {
        return Fail(error, "Could not delete the AppData shadPS4 directory" +
                               (ec ? ": " + ec.message()
                                   : exists_error ? ": " + exists_error.message() : "."));
    }
    return true;
}

} // namespace Common::FS
