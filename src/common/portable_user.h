// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace Common::FS {

enum class PortableCleanupMode {
    None,
    Automatic,
    Ask,
};

/// Copies and verifies the standard shadPS4 directory before switching to portable paths.
[[nodiscard]] bool MigrateToPortableUserDirectory(std::string& error);

[[nodiscard]] PortableCleanupMode GetPortableCleanupMode();

/// Checks that AppData has not changed since the verified copy and that its files still exist.
[[nodiscard]] bool CanRemoveStandardUserDirectory(std::string& error);

/// Persists that the cleanup question has been handled. A completed marker suppresses it forever.
[[nodiscard]] bool CompletePortableCleanup(std::string& error);

/// Revalidates and removes only the platform's exact standard shadPS4 directory.
[[nodiscard]] bool RemoveStandardUserDirectory(std::string& error);

} // namespace Common::FS
