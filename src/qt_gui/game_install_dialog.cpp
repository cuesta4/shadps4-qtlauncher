// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "common/key_manager.h"
#include "common/logging/log.h"
#include "common/portable_user.h"
#include "core/emulator_settings.h"
#include "game_install_dialog.h"
#include "gui_settings.h"

GameInstallDialog::GameInstallDialog() {
    m_gui_settings = std::make_shared<gui_settings>();

    auto layout = new QVBoxLayout(this);

    layout->addWidget(SetupGamesDirectory());
    layout->addWidget(SetupAddonsDirectory());
    layout->addWidget(SetupVersionDirectory());
    layout->addWidget(SetupPortableMode());
    layout->addStretch();
    layout->addWidget(SetupDialogActions());

    setWindowTitle(tr("shadPS4 - Choose directory"));
    setWindowIcon(QIcon(":images/shadps4.ico"));
}

GameInstallDialog::~GameInstallDialog() {}

void GameInstallDialog::BrowseGamesDirectory() {
    auto path = QFileDialog::getExistingDirectory(this, tr("Directory with your dumped games"));

    if (!path.isEmpty()) {
        m_gamesDirectory->setText(QDir::toNativeSeparators(path));
    }
}

void GameInstallDialog::BrowseAddonsDirectory() {
    auto path = QFileDialog::getExistingDirectory(this, tr("Directory with your dumped DLC's"));

    if (!path.isEmpty()) {
        m_addonsDirectory->setText(QDir::toNativeSeparators(path));
    }
}

void GameInstallDialog::BrowseVersionDirectory() {
    auto path =
        QFileDialog::getExistingDirectory(this, tr("Directory to install emulator versions"));

    if (!path.isEmpty()) {
        m_versionDirectory->setText(QDir::toNativeSeparators(path));
    }
}

QWidget* GameInstallDialog::SetupGamesDirectory() {
    auto group = new QGroupBox(tr("Directory with games"));
    auto layout = new QHBoxLayout(group);

    // Input.
    m_gamesDirectory = new QLineEdit();
    QString install_dir;
    std::filesystem::path install_path = EmulatorSettings.GetGameInstallDirs().empty()
                                             ? ""
                                             : EmulatorSettings.GetGameInstallDirs().front();
    Common::FS::PathToQString(install_dir, install_path);
    m_gamesDirectory->setText(install_dir);
    m_gamesDirectory->setMinimumWidth(400);

    layout->addWidget(m_gamesDirectory);

    // Browse button.
    auto browse = new QPushButton(tr("Browse"));

    connect(browse, &QPushButton::clicked, this, &GameInstallDialog::BrowseGamesDirectory);

    layout->addWidget(browse);

    return group;
}

QWidget* GameInstallDialog::SetupAddonsDirectory() {
    auto group = new QGroupBox(tr("Directory with DLC's"));
    auto layout = new QHBoxLayout(group);

    // Input.
    m_addonsDirectory = new QLineEdit();
    QString install_dir;
    Common::FS::PathToQString(install_dir, EmulatorSettings.GetAddonInstallDir());
    m_addonsDirectory->setText(install_dir);
    m_addonsDirectory->setMinimumWidth(400);

    layout->addWidget(m_addonsDirectory);

    // Browse button.
    auto browse = new QPushButton(tr("Browse"));

    connect(browse, &QPushButton::clicked, this, &GameInstallDialog::BrowseAddonsDirectory);

    layout->addWidget(browse);

    return group;
}

QWidget* GameInstallDialog::SetupVersionDirectory() {
    auto group = m_versionDirectoryGroup =
        new QGroupBox(tr("Directory to install emulator versions"));
    auto layout = new QHBoxLayout(group);

    m_versionDirectory = new QLineEdit();
    QString version_dir;
    if (m_gui_settings->GetValue(gui::vm_versionPath).toString().isEmpty()) {
        QString defaultVersionDir = QString::fromStdString(
            Common::FS::GetUserPath(Common::FS::PathType::VersionDir).string());
        m_versionDirectory->setText(defaultVersionDir);
    } else {
        m_versionDirectory->setText(m_gui_settings->GetValue(gui::vm_versionPath).toString());
    }
    m_versionDirectory->setMinimumWidth(400);

    layout->addWidget(m_versionDirectory);

    auto browse = new QPushButton(tr("Browse"));
    connect(browse, &QPushButton::clicked, this, &GameInstallDialog::BrowseVersionDirectory);
    layout->addWidget(browse);

#ifdef HIDE_VERSION_MANAGER
    group->setHidden(true);
#endif

    return group;
}

QWidget* GameInstallDialog::SetupPortableMode() {
    m_enablePortableMode = new QCheckBox(tr("Enable portable mode"));
    m_standardVersionDirectory = m_versionDirectory->text();

    connect(m_enablePortableMode, &QCheckBox::toggled, this, [this](bool enabled) {
        m_versionDirectoryGroup->setEnabled(!enabled);
        if (!enabled) {
            m_versionDirectory->setText(m_standardVersionDirectory);
            return;
        }

        QString portable_version_directory;
        Common::FS::PathToQString(portable_version_directory,
                                  Common::FS::GetApplicationDirectory() / "versions");
        m_versionDirectory->setText(portable_version_directory);
    });
    m_enablePortableMode->setChecked(Common::FS::IsPortableUserDirectory());
    return m_enablePortableMode;
}

QWidget* GameInstallDialog::SetupDialogActions() {
    auto actions = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);

    connect(actions, &QDialogButtonBox::accepted, this, &GameInstallDialog::Save);
    connect(actions, &QDialogButtonBox::rejected, this, &GameInstallDialog::reject);

    return actions;
}

void GameInstallDialog::Save() {
    // Check games directory.
    auto gamesDirectory = m_gamesDirectory->text();
    auto addonsDirectory = m_addonsDirectory->text();
    const bool enable_portable_mode = m_enablePortableMode->isChecked();
    auto versionDirectory = m_versionDirectory->text();
    if (enable_portable_mode) {
        Common::FS::PathToQString(versionDirectory,
                                  Common::FS::GetApplicationDirectory() / "versions");
    }

    if (gamesDirectory.isEmpty() || !QDir(gamesDirectory).exists() ||
        !QDir::isAbsolutePath(gamesDirectory)) {
        QMessageBox::critical(this, tr("Error"),
                              "The choosen location for dumped games is not valid.");
        return;
    }

    if (addonsDirectory.isEmpty() || !QDir::isAbsolutePath(addonsDirectory)) {
        QMessageBox::critical(this, tr("Error"),
                              "The choosen location for dumped DLC's is not valid.");
        return;
    }

    QDir addonsDir(addonsDirectory);
    if (!addonsDir.exists()) {
        if (!addonsDir.mkpath(".")) {
            QMessageBox::critical(this, tr("Error"), "The DLC dump location could not be created.");
            return;
        }
    }

    if (versionDirectory.isEmpty() || !QDir::isAbsolutePath(versionDirectory)) {
        QMessageBox::critical(this, tr("Error"),
                              "The value for location to install emulator versions is not valid.");
        return;
    }

    QDir versionDir(versionDirectory);
    if (!versionDir.exists()) {
        if (!versionDir.mkpath(".")) {
            QMessageBox::critical(this, tr("Error"),
                                  "The emulator version location could not be created.");
            return;
        }
    }

    // Save the directories
    EmulatorSettings.AddGameInstallDir(Common::FS::PathFromQString(gamesDirectory));
    EmulatorSettings.SetAddonInstallDir(Common::FS::PathFromQString(addonsDirectory));
    if (enable_portable_mode) {
        // Keep games and DLCs external, but return every other configurable emulator path to the
        // portable user directory defaults.
        EmulatorSettings.SetHomeDir({});
        EmulatorSettings.SetSysModulesDir({});
        EmulatorSettings.SetFontsDir({});
    }
    m_gui_settings->SetValue(gui::vm_versionPath, versionDirectory);

    if (!EmulatorSettings.Save()) {
        QMessageBox::critical(this, tr("Error"),
                              tr("The emulator configuration could not be saved."));
        return;
    }

    if (enable_portable_mode) {
        m_gui_settings->sync();
        const auto key_manager = KeyManager::GetInstance();
        Common::Log::Flush();
        Common::Log::Shutdown();
        KeyManager::SetInstance(key_manager);

        std::string migration_error;
        if (!Common::FS::MigrateToPortableUserDirectory(migration_error)) {
            Common::Log::Setup("shadPS4Launcher.log");
            QMessageBox::critical(
                this, tr("Portable mode migration failed"),
                tr("Nothing was deleted. The AppData shadPS4 folder was kept.\n\n%1")
                    .arg(QString::fromStdString(migration_error)));
            return;
        }
        Common::Log::Setup("shadPS4Launcher.log");
    }

    accept();
}
