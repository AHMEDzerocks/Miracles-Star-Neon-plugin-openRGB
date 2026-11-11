#include "NeonController.h"

#include <QBluetoothAddress>
#include <QBluetoothUuid>
#include <QMetaObject>
#include <QDebug>
#include <QtGlobal>
#include <algorithm>
#include <cmath>

namespace
{
const QBluetoothUuid kNeonServiceUuid(QStringLiteral("0000fff0-0000-1000-8000-00805f9b34fb"));
const QBluetoothUuid kNeonWriteUuid(QStringLiteral("0000fff3-0000-1000-8000-00805f9b34fb"));
}

NeonController::NeonController(OpenRGBPluginAPIInterface* plugin_api)
    : QObject(nullptr), api(plugin_api)
{
    discovery = new QBluetoothDeviceDiscoveryAgent(this);
    discovery->setLowEnergyDiscoveryTimeout(12000);

    retryTimer = new QTimer(this);
    retryTimer->setSingleShot(true);
    retryTimer->setInterval(5000);
    connect(retryTimer, &QTimer::timeout, this, &NeonController::StartScan);

    sendTimer = new QTimer(this);
    sendTimer->setSingleShot(true);
    sendTimer->setInterval(33); // Up to ~30 FPS; only the newest frame is retained.
    connect(sendTimer, &QTimer::timeout, this, &NeonController::OnSendTimer);

    writeTimer = new QTimer(this);
    writeTimer->setSingleShot(true);
    writeTimer->setInterval(20); // BLE pacing; stale colour frames are coalesced below.
    connect(writeTimer, &QTimer::timeout, this, &NeonController::OnWriteTimer);

    // OpenRGB calls DeviceUpdateMode repeatedly while a slider is dragged.
    // Restart this timer on every update and apply only the final values.
    controlDebounceTimer = new QTimer(this);
    controlDebounceTimer->setSingleShot(true);
    controlDebounceTimer->setInterval(200);
    connect(controlDebounceTimer, &QTimer::timeout,
            this, &NeonController::OnControlDebounceTimer);

    connect(discovery, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered,
            this, &NeonController::OnDeviceDiscovered);
    connect(discovery, &QBluetoothDeviceDiscoveryAgent::finished,
            this, &NeonController::OnScanFinished);
    connect(discovery, &QBluetoothDeviceDiscoveryAgent::canceled,
            this, &NeonController::OnScanFinished);
    connect(discovery, &QBluetoothDeviceDiscoveryAgent::errorOccurred,
            this, [this](QBluetoothDeviceDiscoveryAgent::Error) {
                scanning = false;
                SetStatus(QStringLiteral("Bluetooth scan error: ") + discovery->errorString());
                retryTimer->start();
            });
}

NeonController::~NeonController()
{
    Unregister();
}

QString NeonController::Status() const
{
    return statusText;
}

void NeonController::PowerOn()
{
    // User-verified MR STAR power-on packet: BC 01 01 01 55.
    QMetaObject::invokeMethod(this, [this]() {
        if (!bleService || !txCharacteristic.isValid())
        {
            SetStatus(QStringLiteral("Power On not sent: neon is not connected."));
            return;
        }
        if (sendTimer) sendTimer->stop();
        if (controlDebounceTimer) controlDebounceTimer->stop();
        if (writeTimer) writeTimer->stop();
        packetQueue.clear();
        poweredOff = false;

        QByteArray packet;
        packet.append(static_cast<char>(0xBC));
        packet.append(static_cast<char>(0x01));
        packet.append(static_cast<char>(0x01));
        packet.append(static_cast<char>(0x01));
        packet.append(static_cast<char>(0x55));
        QueuePacket(packet);
        SetStatus(QStringLiteral("Power On command queued; strip should resume its previous mode."));
    }, Qt::QueuedConnection);
}

void NeonController::PowerOff()
{
    // User-verified MR STAR power-off packet: BC 01 01 00 55.
    QMetaObject::invokeMethod(this, [this]() {
        if (!bleService || !txCharacteristic.isValid())
        {
            SetStatus(QStringLiteral("Power Off not sent: neon is not connected."));
            return;
        }
        poweredOff = true;
        if (sendTimer) sendTimer->stop();
        if (controlDebounceTimer) controlDebounceTimer->stop();
        if (writeTimer) writeTimer->stop();
        packetQueue.clear();

        QByteArray packet;
        packet.append(static_cast<char>(0xBC));
        packet.append(static_cast<char>(0x01));
        packet.append(static_cast<char>(0x01));
        packet.append(static_cast<char>(0x00));
        packet.append(static_cast<char>(0x55));
        QueuePacket(packet);
        SetStatus(QStringLiteral("Power Off command queued; RGB/effect updates paused until Turn On."));
    }, Qt::QueuedConnection);
}

void NeonController::MicrophoneMode()
{
    // Protocol table documents microphone mode as: BC 0F 01 01 55.
    QByteArray packet;
    packet.append(static_cast<char>(0xBC));
    packet.append(static_cast<char>(0x0F));
    packet.append(static_cast<char>(0x01));
    packet.append(static_cast<char>(0x01));
    packet.append(static_cast<char>(0x55));
    QMetaObject::invokeMethod(this, [this, packet]() {
        if (!bleService || !txCharacteristic.isValid())
        {
            SetStatus(QStringLiteral("Microphone mode not sent: neon is not connected yet."));
            return;
        }
        QueuePacket(packet);
        SetStatus(QStringLiteral("Microphone mode command queued."));
    }, Qt::QueuedConnection);
}

void NeonController::ToggleDirection()
{
    directionForward = !directionForward;
    const bool forward = directionForward;
    QMetaObject::invokeMethod(this, [this, forward]() {
        if (poweredOff || !bleService || !txCharacteristic.isValid())
        {
            SetStatus(QStringLiteral("Direction not sent: neon is not connected yet."));
            return;
        }
        QueuePacket(MakeDirectionPacket(forward));
        SetStatus(forward ? QStringLiteral("Effect direction: Forward")
                          : QStringLiteral("Effect direction: Reverse"));
    }, Qt::QueuedConnection);
}

void NeonController::SetStatus(const QString& status)
{
    statusText = status;
    qInfo().noquote() << "[MiraclesStarNeon]" << status;
    emit StatusChanged(statusText);
}

void NeonController::Register()
{
    if (!api || rgb_controller)
        return;

    led stripLed;
    stripLed.name = "Neon Strip";
    stripLed.value = 0;
    leds = { stripLed };
    colors = { ToRGBColor(255, 0, 0) };
    pendingColor = colors[0];

    zone stripZone;
    stripZone.name = "Neon Strip";
    stripZone.display_name = "Neon Strip";
    stripZone.type = ZONE_TYPE_LINEAR;
    stripZone.start_idx = 0;
    stripZone.leds_count = 1;
    stripZone.leds_min = 1;
    stripZone.leds_max = 1;
    stripZone.leds = leds.data();
    stripZone.colors = colors.data();
    stripZone.active_mode = 0;
    zones = { stripZone };

    modes.clear();

    mode direct;
    direct.name = "Direct";
    direct.value = 0;
    direct.flags = MODE_FLAG_HAS_PER_LED_COLOR | MODE_FLAG_HAS_BRIGHTNESS |
                   MODE_FLAG_REQUIRES_ENTIRE_DEVICE;
    direct.brightness_min = 3;
    direct.brightness_max = 1000;
    direct.brightness = 1000;
    direct.colors_min = 1;
    direct.colors_max = 1;
    direct.color_mode = MODE_COLORS_PER_LED;
    modes.push_back(direct);

    // Named effects transcribed from GlowBridge's MiraclesStarEffect catalogue,
    // itself attributed to hirotasoshu/ha-findn-led-ble. Keep the full numeric
    // 0-255 list below as well: this catalogue is not asserted to be exhaustive
    // for every controller firmware revision.
    struct NamedEffect { unsigned int id; const char* name; };
    static const NamedEffect namedEffects[] = {
        {0x01, "Automatic loop"},
        {0x02, "Symphony"},
        {0x03, "Colorful energy"},
        {0x04, "Colorful jumps"},
        {0x07, "7 colors strobe"},
        {0x0A, "7 colors gradient"},
        {0x1A, "Colorful fluttering"},
        {0x1B, "Red-green-blue fluttering"},
        {0x1D, "Colorful brushing"},
        {0x1E, "Red-green-blue color brushing"},
        {0x1F, "Yellow-cyan-purple color brushing"},
        {0x20, "Colorful brush / closed-pull"},
        {0x23, "7 colors opening-closing"},
        {0x24, "Red-green-blue opening-closing"},
        {0x25, "Yellow-cyan-purple opening-closing"},
        {0x26, "Red opening-closing"},
        {0x27, "Green opening-closing"},
        {0x28, "Blue opening-closing"},
        {0x29, "Yellow opening-closing"},
        {0x2A, "Cyan opening-closing"},
        {0x2B, "Purple opening-closing"},
        {0x2C, "White opening-closing"},
        {0x2D, "7 colors light-dark transition"},
        {0x2E, "Blue-red-green light-dark transition"},
        {0x2F, "Violet-green-yellow light-dark transition"},
        {0x30, "6 colors light-dark transition (red)"},
        {0x31, "6 colors light-dark transition (green)"},
        {0x32, "6 colors light-dark transition (blue)"},
        {0x33, "6 colors light-dark transition (cyan)"},
        {0x34, "6 colors light-dark transition (yellow)"},
        {0x35, "6 colors light-dark transition (purple)"},
        {0x36, "6 colors light-dark transition (white)"},
        {0x37, "7 colors flowing water"},
        {0x38, "Blue-green-red running water"},
        {0x39, "Purple-green-yellow running water"},
        {0x3A, "Red-green running water"},
        {0x3B, "Green-blue running water"},
        {0x3C, "Yellow-blue running water"},
        {0x3D, "Yellow-cyan running water"},
        {0x3E, "Blue-purple running water"},
        {0x4C, "7 colors tailing"},
        {0x4D, "Red tailing"},
        {0x4E, "Green tailing"},
        {0x4F, "Blue tailing"},
        {0x50, "Yellow tailing"},
        {0x51, "Cyan tailing"},
        {0x52, "Purple tailing"},
        {0x53, "White tailing"},
        {0x54, "Red running"},
        {0x55, "Green running"},
        {0x56, "Blue running"},
        {0x57, "Yellow running"},
        {0x58, "Cyan running"},
        {0x59, "Purple running"},
        {0x5A, "White running"},
        {0x5B, "7 colors running"},
        {0x5C, "Blue-green-red running"},
        {0x5D, "Purple-cyan-yellow running"}
    };

    // Build exactly one OpenRGB mode per protocol ID. Known IDs use their
    // descriptive names; all other IDs remain available as Hardware Effect N
    // so users can discover additional firmware-specific effects later.
    for (unsigned int effectId = 0; effectId <= 255; ++effectId)
    {
        const NamedEffect* named = nullptr;
        for (const auto& candidate : namedEffects)
        {
            if (candidate.id == effectId)
            {
                named = &candidate;
                break;
            }
        }

        mode effect;
        effect.name = named
            ? (std::string(named->name) + " [" + std::to_string(effectId) + "]")
            : ("Hardware Effect " + std::to_string(effectId));
        effect.value = static_cast<int>(effectId);
        effect.flags = MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_BRIGHTNESS |
                       MODE_FLAG_REQUIRES_ENTIRE_DEVICE;
        effect.speed_min = 0;
        effect.speed_max = 100;
        effect.speed = 50;
        effect.brightness_min = 3;
        effect.brightness_max = 1000;
        effect.brightness = 1000;
        effect.colors_min = 0;
        effect.colors_max = 0;
        effect.color_mode = MODE_COLORS_NONE;
        modes.push_back(effect);
    }


    setup = RGBController_Setup{};
    setup.description = "Miracles Star BLE neon strip";
    setup.location = "Bluetooth LE";
    setup.name = "Miracles Star Neon";
    setup.serial = "GATT--DEMO";
    setup.vendor = "Miracles Star";
    setup.version = "0.2.0";
    setup.configuration = "{}";
    setup.active_mode = 0;
    setup.flags = CONTROLLER_FLAG_VIRTUAL;
    setup.type = DEVICE_TYPE_LIGHT;
    setup.modes = modes;
    setup.zones = zones;
    setup.leds = leds;
    setup.object_ptr = this;
    setup.DeviceConfigureZone = nullptr;
    setup.DeviceUpdateLEDs = &NeonController::OnUpdateLEDs;
    setup.DeviceUpdateZoneLEDs = nullptr;
    setup.DeviceUpdateSingleLED = nullptr;
    setup.DeviceUpdateMode = &NeonController::OnUpdateMode;
    setup.DeviceSaveMode = nullptr;
    setup.DeviceUpdateZoneMode = nullptr;
    setup.DeviceUpdateDeviceSpecificConfiguration = nullptr;
    setup.DeviceUpdateDeviceSpecificZoneConfiguration = nullptr;

    rgb_controller = api->CreateVirtualRGBController(&setup);
    if (rgb_controller)
    {
        api->RegisterVirtualRGBController(rgb_controller);
        SetStatus(QStringLiteral("Virtual device registered. Searching for GATT neon..."));
        QTimer::singleShot(0, this, &NeonController::StartScan);
    }
    else
    {
        SetStatus(QStringLiteral("Failed to register the virtual RGB device."));
    }
}

void NeonController::Unregister()
{
    if (retryTimer)
        retryTimer->stop();
    if (sendTimer)
        sendTimer->stop();
    if (writeTimer)
        writeTimer->stop();
    if (controlDebounceTimer)
        controlDebounceTimer->stop();
    packetQueue.clear();
    if (discovery && discovery->isActive())
        discovery->stop();

    CleanupBle(false);

    if (api && rgb_controller)
    {
        api->UnregisterVirtualRGBController(rgb_controller);
        api->DeleteVirtualRGBController(rgb_controller);
        rgb_controller = nullptr;
    }
}

void NeonController::StartScan()
{
    if (!discovery || scanning || bleController)
        return;

    scanning = true;
    SetStatus(QStringLiteral("Searching for BLE device named GATT..."));
    discovery->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
}

void NeonController::OnDeviceDiscovered(const QBluetoothDeviceInfo& info)
{
    const QString name = info.name().trimmed();
    if (!name.startsWith(QStringLiteral("GATT"), Qt::CaseInsensitive))
        return;

    if (discovery && discovery->isActive())
        discovery->stop();
    scanning = false;
    ConnectToDevice(info);
}

void NeonController::OnScanFinished()
{
    scanning = false;
    if (!bleController)
    {
        SetStatus(QStringLiteral("Neon not found. Retrying Bluetooth scan in 5 seconds..."));
        retryTimer->start();
    }
}

void NeonController::ConnectToDevice(const QBluetoothDeviceInfo& info)
{
    if (bleController)
        CleanupBle(false);

    SetStatus(QStringLiteral("Found ") + info.name() + QStringLiteral(". Connecting..."));
    bleController = QLowEnergyController::createCentral(info, this);
    if (!bleController)
    {
        SetStatus(QStringLiteral("Could not create the BLE connection. Retrying..."));
        retryTimer->start();
        return;
    }

    connect(bleController, &QLowEnergyController::connected,
            this, &NeonController::OnBleConnected);
    connect(bleController, &QLowEnergyController::disconnected,
            this, &NeonController::OnBleDisconnected);
    connect(bleController, &QLowEnergyController::serviceDiscovered,
            this, &NeonController::OnServiceDiscovered);
    connect(bleController, &QLowEnergyController::discoveryFinished,
            this, [this]() {
                if (!bleService)
                {
                    SetStatus(QStringLiteral("Connected, but service FFF0 was not found. Retrying..."));
                    CleanupBle(true);
                }
            });
    connect(bleController, &QLowEnergyController::errorOccurred,
            this, [this](QLowEnergyController::Error) {
                SetStatus(QStringLiteral("BLE connection error: ") + bleController->errorString());
                CleanupBle(true);
            });

    bleController->connectToDevice();
}

void NeonController::OnBleConnected()
{
    SetStatus(QStringLiteral("BLE connected. Discovering neon services..."));
    bleController->discoverServices();
}

void NeonController::OnServiceDiscovered(const QBluetoothUuid& uuid)
{
    if (uuid != kNeonServiceUuid || bleService)
        return;

    bleService = bleController->createServiceObject(kNeonServiceUuid, this);
    if (!bleService)
    {
        SetStatus(QStringLiteral("Could not open BLE service FFF0."));
        CleanupBle(true);
        return;
    }

    connect(bleService, &QLowEnergyService::stateChanged,
            this, &NeonController::OnServiceDetailsDiscovered);
    connect(bleService, &QLowEnergyService::errorOccurred,
            this, [this](QLowEnergyService::ServiceError) {
                SetStatus(QStringLiteral("BLE service error."));
                CleanupBle(true);
            });

    SetStatus(QStringLiteral("Found service FFF0. Reading characteristics..."));
    bleService->discoverDetails();
}

void NeonController::OnServiceDetailsDiscovered(QLowEnergyService::ServiceState state)
{
    if (state != QLowEnergyService::RemoteServiceDiscovered || !bleService)
        return;

    txCharacteristic = bleService->characteristic(kNeonWriteUuid);
    if (!txCharacteristic.isValid())
    {
        SetStatus(QStringLiteral("Service FFF0 found, but writable characteristic FFF3 is missing."));
        CleanupBle(true);
        return;
    }

    const auto properties = txCharacteristic.properties();
    if (!(properties & QLowEnergyCharacteristic::Write) &&
        !(properties & QLowEnergyCharacteristic::WriteNoResponse))
    {
        SetStatus(QStringLiteral("Characteristic FFF3 does not support writing."));
        CleanupBle(true);
        return;
    }

    SetStatus(QStringLiteral("Connected to neon. Native BLE color control is ready."));
    if (!packetQueue.isEmpty() && writeTimer && !writeTimer->isActive())
        writeTimer->start(10);
    lastSentColor = 0xFFFFFFFF; // A reconnect must resend the current state.
    lastSentBrightness = 0xFFFFFFFF;
    lastSentSpeed = 0xFFFFFFFF;
    ApplyMode(activeModeIndex, currentBrightness, currentSpeed);
}

void NeonController::OnBleDisconnected()
{
    SetStatus(QStringLiteral("Neon disconnected. Reconnecting..."));
    CleanupBle(true);
}

void NeonController::CleanupBle(bool scheduleRetry)
{
    if (writeTimer)
        writeTimer->stop();
    if (controlDebounceTimer)
        controlDebounceTimer->stop();
    packetQueue.clear();

    if (bleService)
    {
        bleService->deleteLater();
        bleService = nullptr;
    }
    txCharacteristic = QLowEnergyCharacteristic();

    if (bleController)
    {
        QLowEnergyController* oldController = bleController;
        bleController = nullptr;
        if (oldController->state() != QLowEnergyController::UnconnectedState)
            oldController->disconnectFromDevice();
        oldController->deleteLater();
    }

    if (scheduleRetry && retryTimer)
        retryTimer->start();
}

void NeonController::OnUpdateLEDs(void* object)
{
    auto* self = static_cast<NeonController*>(object);
    if (!self || !self->rgb_controller || self->poweredOff)
        return;

    const RGBColor color = self->rgb_controller->GetColor(0);
    self->colors[0] = color;

    // Accept OpenRGB colour frames only in Direct mode. Hardware-effect modes
    // intentionally ignore software LED colour writes.
    const int activeMode = self->rgb_controller->GetActiveMode();
    if (activeMode != 0)
        return;

    QMetaObject::invokeMethod(self, [self, color]() {
        if (!self)
            return;
        self->pendingColor = color;
        self->hasPendingColor = true;
        if (self->sendTimer && !self->sendTimer->isActive())
            self->sendTimer->start();
    }, Qt::QueuedConnection);
}

void NeonController::OnUpdateMode(void* object)
{
    auto* self = static_cast<NeonController*>(object);
    if (!self || !self->rgb_controller || self->poweredOff)
        return;

    const int modeIndex = self->rgb_controller->GetActiveMode();
    if (modeIndex < 0 || modeIndex >= static_cast<int>(self->modes.size()))
        return;

    const unsigned int brightness = self->rgb_controller->GetModeBrightness(modeIndex);
    const unsigned int speed = self->rgb_controller->GetModeSpeed(modeIndex);

    QMetaObject::invokeMethod(self, [self, modeIndex, brightness, speed]() {
        if (!self || self->poweredOff)
            return;

        // Keep only the newest slider values. Without this debounce, dragging
        // brightness/speed can enqueue hundreds of BLE writes and stall OpenRGB.
        self->pendingModeIndex = modeIndex;
        self->pendingBrightness = brightness;
        self->pendingSpeed = speed;
        if (self->controlDebounceTimer)
            self->controlDebounceTimer->start(); // restart = 200 ms after last movement
    }, Qt::QueuedConnection);
}

void NeonController::OnControlDebounceTimer()
{
    ApplyMode(pendingModeIndex, pendingBrightness, pendingSpeed);
}

QByteArray NeonController::MakeColorPacket(unsigned int rgb) const
{
    const int r = static_cast<int>(RGBGetRValue(rgb));
    const int g = static_cast<int>(RGBGetGValue(rgb));
    const int b = static_cast<int>(RGBGetBValue(rgb));

    const double rf = r / 255.0;
    const double gf = g / 255.0;
    const double bf = b / 255.0;
    const double maximum = std::max({ rf, gf, bf });
    const double minimum = std::min({ rf, gf, bf });
    const double delta = maximum - minimum;

    double hue = 0.0;
    double saturation = (maximum == 0.0) ? 0.0 : delta / maximum;

    if (delta != 0.0)
    {
        if (maximum == rf)
        {
            hue = (gf - bf) / delta;
            if (hue < 0.0)
                hue += 6.0;
        }
        else if (maximum == gf)
        {
            hue = ((bf - rf) / delta) + 2.0;
        }
        else
        {
            hue = ((rf - gf) / delta) + 4.0;
        }
        hue /= 6.0;
    }

    const int hueValue = static_cast<int>(std::lround(hue * 360.0));
    const int saturationValue = static_cast<int>(std::lround(saturation * 1000.0));

    QByteArray packet;
    packet.reserve(10);
    packet.append(static_cast<char>(0xBC));
    packet.append(static_cast<char>(0x04));
    packet.append(static_cast<char>(0x06));
    packet.append(static_cast<char>((hueValue >> 8) & 0xFF));
    packet.append(static_cast<char>(hueValue & 0xFF));
    packet.append(static_cast<char>((saturationValue >> 8) & 0xFF));
    packet.append(static_cast<char>(saturationValue & 0xFF));
    packet.append(static_cast<char>(0x00));
    packet.append(static_cast<char>(0x00));
    packet.append(static_cast<char>(0x55));
    return packet;
}

QByteArray NeonController::MakeBrightnessPacket(unsigned int brightness) const
{
    brightness = std::clamp(brightness, 3u, 1000u);
    QByteArray packet;
    packet.reserve(10);
    packet.append(static_cast<char>(0xBC));
    packet.append(static_cast<char>(0x05));
    packet.append(static_cast<char>(0x06));
    packet.append(static_cast<char>((brightness >> 8) & 0xFF));
    packet.append(static_cast<char>(brightness & 0xFF));
    packet.append(static_cast<char>(0x00));
    packet.append(static_cast<char>(0x00));
    packet.append(static_cast<char>(0x00));
    packet.append(static_cast<char>(0x00));
    packet.append(static_cast<char>(0x55));
    return packet;
}

QByteArray NeonController::MakeEffectPacket(unsigned int effectId) const
{
    const unsigned int high = effectId / 255u;
    const unsigned int low = effectId % 255u;
    QByteArray packet;
    packet.reserve(6);
    packet.append(static_cast<char>(0xBC));
    packet.append(static_cast<char>(0x06));
    packet.append(static_cast<char>(0x02));
    packet.append(static_cast<char>(high & 0xFF));
    packet.append(static_cast<char>(low & 0xFF));
    packet.append(static_cast<char>(0x55));
    return packet;
}

QByteArray NeonController::MakeDirectionPacket(bool forward) const
{
    // Miracles Star direction: BC 07 01 01 55 = forward,
    // BC 07 01 00 55 = reverse.
    QByteArray packet;
    packet.reserve(5);
    packet.append(static_cast<char>(0xBC));
    packet.append(static_cast<char>(0x07));
    packet.append(static_cast<char>(0x01));
    packet.append(static_cast<char>(forward ? 0x01 : 0x00));
    packet.append(static_cast<char>(0x55));
    return packet;
}

QByteArray NeonController::MakeSpeedPacket(unsigned int speed) const
{
    speed = std::min(speed, 100u);
    QByteArray packet;
    packet.reserve(5);
    packet.append(static_cast<char>(0xBC));
    packet.append(static_cast<char>(0x08));
    packet.append(static_cast<char>(0x01));
    packet.append(static_cast<char>(speed & 0xFF));
    packet.append(static_cast<char>(0x55));
    return packet;
}

void NeonController::QueuePacket(const QByteArray& packet)
{
    if (packet.isEmpty())
        return;

    // Colour frames are disposable: never let an old animation frame sit in
    // front of the newest one. Command 0x04 is the global-colour packet.
    if (packet.size() >= 2 && static_cast<unsigned char>(packet[1]) == 0x04)
    {
        for (int i = packetQueue.size() - 1; i >= 0; --i)
        {
            const QByteArray& queued = packetQueue.at(i);
            if (queued.size() >= 2 && static_cast<unsigned char>(queued[1]) == 0x04)
                packetQueue.removeAt(i);
        }
    }

    packetQueue.enqueue(packet);
    if (writeTimer && !writeTimer->isActive())
        writeTimer->start(1);
}

void NeonController::SendNextPacket()
{
    if (packetQueue.isEmpty())
        return;
    if (!bleService || !txCharacteristic.isValid())
        return; // Keep the packet queued; connection-ready will restart the timer.

    const QByteArray packet = packetQueue.dequeue();
    const auto properties = txCharacteristic.properties();
    // Prefer Write Without Response when the characteristic supports it.
    // This matches the low-latency Python bridge and avoids waiting for an
    // ATT acknowledgement for every colour frame.
    const QLowEnergyService::WriteMode writeMode =
        (properties & QLowEnergyCharacteristic::WriteNoResponse)
            ? QLowEnergyService::WriteWithoutResponse
            : QLowEnergyService::WriteWithResponse;
    bleService->writeCharacteristic(txCharacteristic, packet, writeMode);

    // Keep a modest interval for the controller's BLE receiver. Since colour
    // packets are coalesced, this cannot build an animation backlog.
    if (!packetQueue.isEmpty() && writeTimer)
        writeTimer->start(20);
}

void NeonController::ApplyMode(int modeIndex, unsigned int brightness, unsigned int speed)
{
    if (poweredOff || modeIndex < 0 || modeIndex >= static_cast<int>(modes.size()))
        return;

    activeModeIndex = modeIndex;
    currentBrightness = std::clamp(brightness, 3u, 1000u);
    currentSpeed = std::min(speed, 100u);

    if (!bleService || !txCharacteristic.isValid())
        return;

    // A mode switch invalidates any unsent frames/commands from the previous
    // mode. Drop them so the newly selected effect takes priority immediately.
    packetQueue.clear();
    if (writeTimer)
        writeTimer->stop();

    const bool softwareColorMode = (modeIndex == 0);
    if (softwareColorMode)
    {
        // Direct mode cancels the strip's hardware effect and accepts software colours.
        QueuePacket(MakeColorPacket(pendingColor));
        lastSentColor = pendingColor;
        hasPendingColor = false;
    }
    else
    {
        const unsigned int effectId = static_cast<unsigned int>(modes[modeIndex].value);
        QueuePacket(MakeEffectPacket(effectId));
        QueuePacket(MakeDirectionPacket(directionForward));
    }

    if (currentBrightness != lastSentBrightness)
    {
        QueuePacket(MakeBrightnessPacket(currentBrightness));
        lastSentBrightness = currentBrightness;
    }

    // Elkotrol capture confirms command 0x08 sets effect speed (0-100).
    // Expose and apply speed for every hardware effect, including named IDs.
    if (modeIndex != 0 && currentSpeed != lastSentSpeed)
    {
        QueuePacket(MakeSpeedPacket(currentSpeed));
        lastSentSpeed = currentSpeed;
    }

}

void NeonController::SendCurrentColor()
{
    const bool softwareColorMode = (activeModeIndex == 0);
    if (poweredOff || !softwareColorMode || !bleService || !txCharacteristic.isValid() || !hasPendingColor)
        return;

    if (pendingColor == lastSentColor)
    {
        hasPendingColor = false;
        return;
    }

    QueuePacket(MakeColorPacket(pendingColor));
    lastSentColor = pendingColor;
    hasPendingColor = false;
}

void NeonController::OnSendTimer()
{
    SendCurrentColor();
}

void NeonController::OnWriteTimer()
{
    SendNextPacket();
}
