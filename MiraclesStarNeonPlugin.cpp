#include "MiraclesStarNeonPlugin.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>

OpenRGBPluginInfo MiraclesStarNeonPlugin::GetPluginInfo()
{
    OpenRGBPluginInfo info;
    info.Name = "Miracles Star Neon";
    info.Description = "Native Bluetooth LE integration for Miracles Star neon strips";
    info.Version = "0.2.1";
    info.Commit = "local";
    info.URL = "https://github.com/";
    info.Location = OPENRGB_PLUGIN_LOCATION_TOP;
    info.Label = "Miracles Star";
    info.ProtocolVersion = 0;
    return info;
}

unsigned int MiraclesStarNeonPlugin::GetPluginAPIVersion()
{
    return OPENRGB_PLUGIN_API_VERSION;
}

void MiraclesStarNeonPlugin::Load(OpenRGBPluginAPIInterface* api_interface_ptr)
{
    api = api_interface_ptr;
    controller = new NeonController(api);
    controller->Register();
}

QWidget* MiraclesStarNeonPlugin::GetWidget()
{
    if (!widget)
    {
        widget = new QWidget();
        auto* layout = new QVBoxLayout(widget);
        layout->addWidget(new QLabel(
            "Miracles Star Neon\n"
            "Native Bluetooth LE control is enabled.\n"
            "Static Color sends RGB; named and numeric modes select built-in strip effects.\n"
            "Use Reverse Direction to change the direction of the active effect."));

        statusLabel = new QLabel(widget);
        statusLabel->setWordWrap(true);
        if (controller)
        {
            statusLabel->setText(controller->Status());
            connect(controller, &NeonController::StatusChanged,
                    statusLabel, &QLabel::setText);
        }
        else
        {
            statusLabel->setText(QStringLiteral("Controller is not initialized."));
        }
        layout->addWidget(statusLabel);

        auto* powerLayout = new QHBoxLayout();
        auto* onButton = new QPushButton(QStringLiteral("Turn On"), widget);
        auto* offButton = new QPushButton(QStringLiteral("Turn Off"), widget);
        auto* micButton = new QPushButton(QStringLiteral("Microphone Mode"), widget);
        auto* reverseButton = new QPushButton(QStringLiteral("Reverse Direction"), widget);
        onButton->setEnabled(controller != nullptr);
        offButton->setEnabled(controller != nullptr);
        micButton->setEnabled(controller != nullptr);
        reverseButton->setEnabled(controller != nullptr);
        if (controller)
        {
            connect(onButton, &QPushButton::clicked, controller, &NeonController::PowerOn);
            connect(offButton, &QPushButton::clicked, controller, &NeonController::PowerOff);
            connect(micButton, &QPushButton::clicked, controller, &NeonController::MicrophoneMode);
            connect(reverseButton, &QPushButton::clicked, controller, &NeonController::ToggleDirection);
        }
        powerLayout->addWidget(onButton);
        powerLayout->addWidget(offButton);
        powerLayout->addWidget(micButton);
        powerLayout->addWidget(reverseButton);
        layout->addLayout(powerLayout);
        layout->addStretch();
    }
    return widget;
}

QMenu* MiraclesStarNeonPlugin::GetTrayMenu()
{
    return nullptr;
}

void MiraclesStarNeonPlugin::Unload()
{
    if (controller)
    {
        controller->Unregister();
        delete controller;
        controller = nullptr;
    }

    delete widget;
    widget = nullptr;
    statusLabel = nullptr;
    api = nullptr;
}

void MiraclesStarNeonPlugin::OnProfileAboutToLoad() {}
void MiraclesStarNeonPlugin::OnProfileLoad(nlohmann::json) {}
nlohmann::json MiraclesStarNeonPlugin::OnProfileSave() { return nlohmann::json::object(); }
unsigned char* MiraclesStarNeonPlugin::OnSDKCommand(unsigned int, unsigned char*, unsigned int*) { return nullptr; }
void MiraclesStarNeonPlugin::ProfileManagerUpdated(unsigned int) {}
void MiraclesStarNeonPlugin::ResourceManagerUpdated(unsigned int) {}
void MiraclesStarNeonPlugin::SettingsManagerUpdated(unsigned int) {}
