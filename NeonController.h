#pragma once

#include <QObject>
#include <QByteArray>
#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothDeviceInfo>
#include <QLowEnergyController>
#include <QLowEnergyService>
#include <QTimer>
#include <QQueue>
#include <vector>

#include "OpenRGBPluginInterface.h"

class NeonController : public QObject
{
    Q_OBJECT

public:
    explicit NeonController(OpenRGBPluginAPIInterface* plugin_api);
    ~NeonController() override;

    void Register();
    void Unregister();
    QString Status() const;
    void PowerOn();
    void PowerOff();
    void MicrophoneMode();
    void ToggleDirection();

signals:
    void StatusChanged(const QString& status);

private slots:
    void StartScan();
    void OnDeviceDiscovered(const QBluetoothDeviceInfo& info);
    void OnScanFinished();
    void OnBleConnected();
    void OnServiceDiscovered(const QBluetoothUuid& uuid);
    void OnServiceDetailsDiscovered(QLowEnergyService::ServiceState state);
    void OnBleDisconnected();
    void OnSendTimer();
    void OnWriteTimer();
    void OnControlDebounceTimer();

private:
    static void OnUpdateLEDs(void* object);
    static void OnUpdateMode(void* object);

    void SetStatus(const QString& status);
    void ConnectToDevice(const QBluetoothDeviceInfo& info);
    void SendCurrentColor();
    void ApplyMode(int modeIndex, unsigned int brightness, unsigned int speed);
    void QueuePacket(const QByteArray& packet);
    void SendNextPacket();
    QByteArray MakeColorPacket(unsigned int rgb) const;
    QByteArray MakeBrightnessPacket(unsigned int brightness) const;
    QByteArray MakeEffectPacket(unsigned int effectId) const;
    QByteArray MakeDirectionPacket(bool forward) const;
    QByteArray MakeSpeedPacket(unsigned int speed) const;
    void CleanupBle(bool scheduleRetry);

    OpenRGBPluginAPIInterface* api = nullptr;
    RGBControllerInterface* rgb_controller = nullptr;

    std::vector<led> leds;
    std::vector<zone> zones;
    std::vector<RGBColor> colors;
    std::vector<mode> modes;
    RGBController_Setup setup{};

    QBluetoothDeviceDiscoveryAgent* discovery = nullptr;
    QLowEnergyController* bleController = nullptr;
    QLowEnergyService* bleService = nullptr;
    QLowEnergyCharacteristic txCharacteristic;
    QTimer* retryTimer = nullptr;
    QTimer* sendTimer = nullptr;
    QTimer* writeTimer = nullptr;
    QTimer* controlDebounceTimer = nullptr;
    QQueue<QByteArray> packetQueue;

    QString statusText = QStringLiteral("Initializing Bluetooth LE...");
    unsigned int pendingColor = 0x0000FF; // OpenRGB RGBColor format: R in low byte.
    unsigned int lastSentColor = 0xFFFFFFFF;
    bool hasPendingColor = true;
    bool scanning = false;
    int activeModeIndex = 0;
    unsigned int currentBrightness = 1000;
    unsigned int currentSpeed = 50;
    bool directionForward = true;
    bool poweredOff = false;
    unsigned int lastSentBrightness = 0xFFFFFFFF;
    unsigned int lastSentSpeed = 0xFFFFFFFF;
    int pendingModeIndex = 0;
    unsigned int pendingBrightness = 1000;
    unsigned int pendingSpeed = 50;
};
