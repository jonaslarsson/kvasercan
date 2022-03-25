/****************************************************************************
**
** Copyright (C) 2017 Denis Shienkov <denis.shienkov@gmail.com>
** Copyright (C) 2019 Andre Hartmann <aha_1980@gmx.de>
** Copyright (C) 2021 Jonas Larsson <jonas.larsson@systemrefine.com>
** Contact: http://www.qt.io/licensing/
**
** This file is part of the QtSerialBus module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL3$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPLv3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or later as published by the Free
** Software Foundation and appearing in the file LICENSE.GPL included in
** the packaging of this file. Please review the following information to
** ensure the GNU General Public License version 2.0 requirements will be
** met: http://www.gnu.org/licenses/gpl-2.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "kvasercanbackend.h"

#include <QtSerialBus/qcanbusdevice.h>

#include <QtCore/qcoreevent.h>
#include <QtCore/qloggingcategory.h>
#include <QtCore/qtimer.h>
#include <QtCore/qlibrary.h>

#include <algorithm>

QT_BEGIN_NAMESPACE

Q_DECLARE_LOGGING_CATEGORY(QT_CANBUS_PLUGINS_KVASERCAN)

#ifndef LINK_LIBKVASERCAN
Q_GLOBAL_STATIC(QLibrary, kvasercanLibrary)
#endif

// WARNING: This function is called from a high priority thread within CANLIB.
//          Sending a message to Qt on every event WILL hang the Qt event loop
//          under high bus loads, since events will be coming in faster than
//          they can be processed.
static void WINAPI callbackHandler(KvaserHandle, void *internalPointer, quint32 eventFlags)
{
    auto backend = static_cast<KvaserCanBackend*>(internalPointer);
    if (eventFlags & KVASER_NOTIFY_RX)
        backend->setMessagesAvailable();
    if (eventFlags & KVASER_NOTIFY_ERROR)
        backend->setMessagesAvailable();
    if (eventFlags & KVASER_NOTIFY_STATUS)
        QMetaObject::invokeMethod(backend, &KvaserCanBackend::onStatusChanged, Qt::QueuedConnection);
    if (eventFlags & KVASER_NOTIFY_BUSONOFF)
        QMetaObject::invokeMethod(backend, &KvaserCanBackend::onBusOnOff, Qt::QueuedConnection);
    if (eventFlags & KVASER_NOTIFY_REMOVED)
        QMetaObject::invokeMethod(backend, &KvaserCanBackend::onDeviceRemoved, Qt::QueuedConnection);
}

static QString systemErrorString(KvaserStatus errorCode)
{
    char buffer[256];
    KvaserStatus result = canGetErrorText(errorCode, buffer, sizeof(buffer));
    if (result == KvaserStatus::OK)
        return QString::fromLatin1(buffer);
    return KvaserCanBackend::tr("Unable to retrieve an error string");
}

static bool getUniqueChannelId(int channel, QString *uniqueId)
{
    quint64 serial = 0;
    if (canGetChannelData(channel, KvaserCanGetChannelDataItem::CardSerialNumber, &serial, sizeof(serial)) != KvaserStatus::OK)
        return false;

    quint32 channelOnCard = 0;
    if (canGetChannelData(channel, KvaserCanGetChannelDataItem::CardChannelNumber, &channelOnCard, sizeof(channelOnCard)) != KvaserStatus::OK)
        return false;

    quint8 deviceEan[8];
    if (canGetChannelData(channel, KvaserCanGetChannelDataItem::CardUpcNumber, deviceEan, sizeof(deviceEan)) != KvaserStatus::OK)
        return false;

    QString eanNumber;
    for (int i = 0; i < 8; i++) {
        char c = '0' + (deviceEan[i] & 0x0F);
        eanNumber.prepend(c);
        c = '0' + (deviceEan[i] >> 4);
        eanNumber.prepend(c);
    }

    *uniqueId = eanNumber + "#" + QString::number(serial) + "." +
            QString::number(channelOnCard);

    return true;
}

KvaserCanBackend::KvaserCanBackend(const QString &name, QObject *parent) : QCanBusDevice(parent)
{
    setupChannel(name);
    setupDefaultConfigurations();
}

KvaserCanBackend::~KvaserCanBackend()
{
    KvaserCanBackend::close();
}

bool KvaserCanBackend::open()
{
    int channelCount = 0;
    KvaserStatus result = canEnumHardwareEx(&channelCount);
    if (Q_UNLIKELY(result != KvaserStatus::OK)) {
        const QString errorString = systemErrorString(result);
        qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Failed to get devices: %ls.", qUtf16Printable(errorString));
        setError(errorString, CanBusError::ConnectionError);
        return false;
    }

    int channelIndex = -1;
    for (int channel = 0; channel < channelCount; ++channel) {
        QString uniqueId;
        if (Q_LIKELY(getUniqueChannelId(channel, &uniqueId)))
        {
            if (uniqueId == m_interfaceName) {
                channelIndex = channel;
                break;
            }
        }
    }

    if (Q_UNLIKELY(channelIndex < 0)) {
        qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Interface not available: %ls.", qUtf16Printable(m_interfaceName));
        setError(tr("Interface not available"), CanBusError::ConnectionError);
        return false;
    }

    int flags = KVASER_OPEN_ACCEPT_VIRTUAL;
    if (m_canFd)
        flags |= KVASER_OPEN_CANFD;

    m_initAccess = true;
    m_kvaserHandle = canOpenChannel(channelIndex, flags | KVASER_OPEN_REQUIRE_INIT_ACCESS);

    if (m_kvaserHandle < 0) {
        m_initAccess = false;
        qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Could NOT get init access, won't be able to set bitrate configuratin etc.");
        m_kvaserHandle = canOpenChannel(channelIndex, flags | KVASER_OPEN_NO_INIT_ACCESS);
    }

    if (Q_UNLIKELY(m_kvaserHandle < 0)) {
        const QString errorString = systemErrorString((KvaserStatus)m_kvaserHandle);
        qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Failed to open channel: %ls.", qUtf16Printable(errorString));
        setError(errorString, CanBusError::ConnectionError);
        return false;
    }

    result = kvSetNotifyCallback(m_kvaserHandle, callbackHandler, this,
                                 KVASER_NOTIFY_RX | KVASER_NOTIFY_BUSONOFF |
                                 KVASER_NOTIFY_REMOVED | KVASER_NOTIFY_STATUS);
    if (Q_UNLIKELY(result != KvaserStatus::OK)) {
        const QString errorString = systemErrorString(result);
        qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Failed to set notify callback: %ls.", qUtf16Printable(errorString));
        setError(errorString, CanBusError::ConnectionError);
        return false;
    }

    const auto keys = configurationKeys();
    for (ConfigurationKey key : keys) {
        const QVariant param = configurationParameter(key);
        const bool success = applyConfigurationParameter(key, param);
        if (!success) {
            qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Cannot apply parameter: %d with value: %ls.",
                      key, qUtf16Printable(param.toString()));
        }
    }

    if (!setDriverMode(KvaserDriverMode::Normal)) {
        close();
        return false;
    }

    if (!setBusOn()) {
        close();
        return false;
    }

    setState(ConnectedState);

    return true;
}

void KvaserCanBackend::close()
{
    if (m_kvaserHandle >= 0)
        canClose(m_kvaserHandle);
    m_kvaserHandle = -1;
    setState(UnconnectedState);
}

void KvaserCanBackend::setConfigurationParameter(ConfigurationKey key, const QVariant &value)
{
    if (applyConfigurationParameter(key, value))
        QCanBusDevice::setConfigurationParameter(key, value);
}

bool KvaserCanBackend::writeFrame(const QCanBusFrame &frame)
{
    if (state() != ConnectedState)
        return false;

    if (m_kvaserHandle < 0)
        return false;

    if (Q_UNLIKELY(!frame.isValid())) {
        setError(tr("Cannot write invalid QCanBusFrame"), WriteError);
        return false;
    }

    if (Q_UNLIKELY(frame.frameType() != QCanBusFrame::DataFrame
            && frame.frameType() != QCanBusFrame::RemoteRequestFrame
            && frame.frameType() != QCanBusFrame::ErrorFrame)) {
        setError(tr("Unable to write a frame with unacceptable type"),
                 WriteError);
        return false;
    }

    const QByteArray payload = frame.payload();

    quint32 flags = 0;
    if (frame.frameType() == QCanBusFrame::RemoteRequestFrame)
        flags |= KVASER_MESSAGE_REMOTE_REQUEST;
    else if (frame.frameType() == QCanBusFrame::ErrorFrame)
        flags |= KVASER_MESSAGE_ERROR_FRAME;
    else
        flags = 0;

    if (frame.hasExtendedFrameFormat())
        flags |= KVASER_MESSAGE_EXTENDED_FRAME_FORMAT;
    else
        flags |= KVASER_MESSAGE_STANDARD_FRAME_FORMAT;

    if (frame.hasFlexibleDataRateFormat())
        flags |= KVASER_MESSAGE_CANFD;

    if (frame.hasBitrateSwitch())
        flags |= KVASER_MESSAGE_BIT_RATE_SWITCH;

    KvaserStatus result = canWrite(m_kvaserHandle, frame.frameId(), payload, payload.size(), flags);

    if (result != KvaserStatus::OK) {
        setError(systemErrorString(result), WriteError);
        return false;
    }

    return true;
}

QString KvaserCanBackend::interpretErrorFrame(const QCanBusFrame &errorFrame)
{
    if (errorFrame.frameType() != QCanBusFrame::ErrorFrame)
        return QString();
    // TODO: Implement this
    return {};
}

bool KvaserCanBackend::canCreate(QString *errorReason)
{
#ifndef LINK_LIBKVASERCAN
    static bool symbolsResolved = resolveKvaserCanSymbols(kvasercanLibrary(), errorReason);
    if (Q_UNLIKELY(!symbolsResolved))
        return false;
    if (canEnumHardwareEx == nullptr) {
        qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Old version of CANLIB detected. Plugging in hardware after the program has started is not supported.");
        canEnumHardwareEx = canGetNumberOfChannels;
    }
#endif
    canInitializeLibrary();
    return true;
}

QList<QCanBusDeviceInfo> KvaserCanBackend::interfaces()
{
    int channelCount = 0;
    if (canEnumHardwareEx(&channelCount) != KvaserStatus::OK) {
        qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Cannot get number of channels");
        return QList<QCanBusDeviceInfo>();
    }

    int numActual = 0;
    for (int channel = 0; channel < channelCount; ++channel) {
        quint32 capabilities = 0;
        if (canGetChannelData(channel, KvaserCanGetChannelDataItem::Capabilities, &capabilities, sizeof(capabilities)) != KvaserStatus::OK)
            continue;
        const bool isVirtual = capabilities & KVASER_CAPABILITY_VIRTUAL;

        if (!isVirtual)
            ++numActual;
    }

    QList<QCanBusDeviceInfo> result;
    for (int channel = 0; channel < channelCount; ++channel) {
        char name[256];
        if (canGetChannelData(channel, KvaserCanGetChannelDataItem::DeviceProductName, &name, sizeof(name)) != KvaserStatus::OK)
            continue;

        quint64 serial = 0;
        if (canGetChannelData(channel, KvaserCanGetChannelDataItem::CardSerialNumber, &serial, sizeof(serial)) != KvaserStatus::OK)
            continue;

        quint32 channelOnCard = 0;
        if (canGetChannelData(channel, KvaserCanGetChannelDataItem::CardChannelNumber, &channelOnCard, sizeof(channelOnCard)) != KvaserStatus::OK)
            continue;

        quint32 capabilities = 0;
        if (canGetChannelData(channel, KvaserCanGetChannelDataItem::Capabilities, &capabilities, sizeof(capabilities)) != KvaserStatus::OK)
            continue;

        // Channel numbers change when devices are plugged in or removed, use
        // unique name based on EAN and serial number instead of "can<n>", so that
        // the device identifier is always the same.
        QString uniqueId;
        if (getUniqueChannelId(channel, &uniqueId) == false)
            continue;

        const bool isVirtual = capabilities & KVASER_CAPABILITY_VIRTUAL;

        // Currently no support for CAN FD devices
        bool isCanFd = false;
        if (capabilities & KVASER_CAPABILITY_CANFD)
            isCanFd = true;

        QString description = QLatin1String(name);
        if (!isVirtual && numActual > 1) {
            description += " Channel " + QString::number(channelOnCard + 1);
        }

        const QString alias;
        const QCanBusDeviceInfo info = createDeviceInfo(QStringLiteral("kvasercan"),
                                                        uniqueId,
                                                        QString::number(serial),
                                                        description,
                                                        alias,
                                                        int(channelOnCard),
                                                        isVirtual, isCanFd);
        result.append(std::move(info));
    }

    return result;
}

QCanBusDevice::CanBusStatus KvaserCanBackend::busStatus()
{
    if (m_kvaserHandle < 0)
        return CanBusStatus::Unknown;
    quint32 flags = 0;
    KvaserStatus result = canReadStatus(m_kvaserHandle, &flags);
    if (result != KvaserStatus::OK) {
        const QString errorString = systemErrorString(result);
        qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Can not query CAN bus status: %ls.",
            qUtf16Printable(errorString));
        setError(errorString, CanBusError::ReadError);
        return CanBusStatus::Unknown;
    }
    if (flags & KVASER_STATUS_BUSOFF)
        return CanBusStatus::BusOff;
    else if (flags & KVASER_STATUS_ERROR_PASSIVE)
        return CanBusStatus::Error;
    else if (flags & KVASER_STATUS_ERROR_WARNING)
        return CanBusStatus::Warning;
    else if (flags & KVASER_STATUS_ERROR_ACTIVE)
        return CanBusStatus::Good;

    qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Unknown CAN bus status flags: 0x%08x", flags);
    return CanBusStatus::Unknown;
}

void KvaserCanBackend::resetController()
{
    if (m_kvaserHandle < 0)
        return;
    KvaserStatus result = canResetBus(m_kvaserHandle);
    if (result != KvaserStatus::OK) {
        const QString errorString = systemErrorString(result);
        qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Failed to reset can bus: %ls.",
            qUtf16Printable(errorString));
        setError(errorString, CanBusError::ReadError);
    }
}

void KvaserCanBackend::onMessagesAvailable()
{
    QList<QCanBusFrame> newFrames;

    m_messagesAvailable = false;

    for (;;) {
        quint32 frameId = 0;
        char buffer[64];
        quint32 dlc = 0;
        quint32 flags = 0;
        quint32 time = 0;
        KvaserStatus result = canRead(m_kvaserHandle, &frameId, buffer, &dlc, &flags, &time);
        if (result == KvaserStatus::NoMessages)
            break;
        if (result != KvaserStatus::OK) {
            setError(systemErrorString(result), ReadError);
            break;
        }
        QCanBusFrame frame;
        frame.setTimeStamp(QCanBusFrame::TimeStamp::fromMicroSeconds((qint64)time * 1000));
        frame.setFrameType(QCanBusFrame::DataFrame);
        if (flags & KVASER_MESSAGE_REMOTE_REQUEST)
            frame.setFrameType(QCanBusFrame::RemoteRequestFrame);
        if (flags & KVASER_MESSAGE_ERROR_FRAME)
            frame.setFrameType(QCanBusFrame::ErrorFrame);
        frame.setExtendedFrameFormat(flags & KVASER_MESSAGE_EXTENDED_FRAME_FORMAT);
        frame.setBitrateSwitch(flags & KVASER_MESSAGE_CANFD);
        frame.setFlexibleDataRateFormat(flags & KVASER_MESSAGE_BIT_RATE_SWITCH);
        frame.setFrameId(frameId);
        frame.setPayload(QByteArray(buffer, dlc));
        newFrames.append(frame);
    }

    enqueueReceivedFrames(newFrames);
}

void KvaserCanBackend::onStatusChanged()
{
#if 0
    quint32 flags = 0;
    KvaserStatus result = canReadStatus(m_kvaserHandle, &flags);
    if (result == KvaserStatus::OK) {
        qDebug() << Q_FUNC_INFO
                 << (flags & KVASER_STATUS_ERROR_PASSIVE ? "ERROR_PASSIVE" : "")
                 << (flags & KVASER_STATUS_BUSOFF ? "BUS_OFF" : "")
                 << (flags & KVASER_STATUS_ERROR_WARNING ? "ERROR_WARNING" : "")
                 << (flags & KVASER_STATUS_ERROR_ACTIVE ? "ERROR_ACTIVE" : "")
                 << (flags & KVASER_STATUS_TX_PENDING ? "TX_PENDING" : "")
                 << (flags & KVASER_STATUS_RX_PENDING ? "RX_PENDING" : "")
                 << (flags & KVASER_STATUS_TX_ERROR ? "TX_ERROR" : "")
                 << (flags & KVASER_STATUS_RX_ERROR ? "RX_ERROR" : "")
                 << (flags & KVASER_STATUS_HW_OVERRUN ? "HW_OVERRUN" : "")
                 << (flags & KVASER_STATUS_SW_OVERRUN ? "SW_OVERRUN" : "");
    } else {
        setError(systemErrorString(result), ConnectionError);
    }
#endif
}

void KvaserCanBackend::onBusOnOff()
{
    quint32 flags = 0;
    KvaserStatus result = canReadStatus(m_kvaserHandle, &flags);
    if (result == KvaserStatus::OK) {
        if (flags & KVASER_STATUS_BUSOFF) {
            setError(tr("Bus off"), ConnectionError);
        }
    } else {
        setError(systemErrorString(result), ReadError);
    }
}

void KvaserCanBackend::onDeviceRemoved()
{
    close();
}

bool KvaserCanBackend::applyConfigurationParameter(QCanBusDevice::ConfigurationKey key, const QVariant &value)
{
    switch (key) {
    case ReceiveOwnKey:
        return setReceiveOwnKey(value.toBool());
    case LoopbackKey:
        return setLoopback(value.toBool());
    case RawFilterKey:
    {
        const QList<Filter> filterList = value.value<QList<Filter> >();
        return setFilters(filterList);
    }
    case BitRateKey:
        return setBitRate(value.toUInt());
    case CanFdKey:
        return setCanFd(value.toBool());
    case DataBitRateKey:
        return setDataBitRate(value.toUInt());
    default:
        setError(tr("Unsupported configuration key: %1").arg(key), ConfigurationError);
        return false;
    }
}

void KvaserCanBackend::setupChannel(const QString &interfaceName)
{
    m_interfaceName = interfaceName;
}

void KvaserCanBackend::setupDefaultConfigurations()
{
    KvaserCanBackend::setConfigurationParameter(BitRateKey, 500000);
}

bool KvaserCanBackend::setReceiveOwnKey(bool enable)
{
    if (updateSettingsAllowed()) {
        quint32 receiveOwnKey = enable ? 1 : 0;
        KvaserStatus result = canIoCtl(m_kvaserHandle, KVASER_IOCTL_RECEIVE_OWN_KEY, &receiveOwnKey, sizeof(receiveOwnKey));
        if (result != KvaserStatus::OK) {
            const QString errorString = systemErrorString(result);
            setError(errorString, ConfigurationError);
            qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Failed to set receive own key: %ls", qUtf16Printable(errorString));
            return false;
        }
    }
    return true;
}

bool KvaserCanBackend::setLoopback(bool enable)
{
    if (updateSettingsAllowed()) {
        char transmitEcho = enable ? 1 : 0;
        KvaserStatus result = canIoCtl(m_kvaserHandle, KVASER_IOCTL_SET_LOOPBACK, &transmitEcho, sizeof(transmitEcho));
        if (result != KvaserStatus::OK) {
            const QString errorString = systemErrorString(result);
            setError(errorString, ConfigurationError);
            qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Failed to set loopback: %ls", qUtf16Printable(errorString));
            return false;
        }
    }
    return true;
}

bool KvaserCanBackend::setBitRate(quint32 bitrate)
{
    qint32 kvaserBitRate;
    switch (bitrate) {
    case 10000:
        kvaserBitRate = KVASER_BITRATE_10K;
        break;
    case 50000:
        kvaserBitRate = KVASER_BITRATE_50K;
        break;
    case 62000:
        kvaserBitRate = KVASER_BITRATE_62K;
        break;
    case 83000:
        kvaserBitRate = KVASER_BITRATE_83K;
        break;
    case 100000:
        kvaserBitRate = KVASER_BITRATE_100K;
        break;
    case 125000:
        kvaserBitRate = KVASER_BITRATE_125K;
        break;
    case 250000:
        kvaserBitRate = KVASER_BITRATE_250K;
        break;
    case 500000:
        kvaserBitRate = KVASER_BITRATE_500K;
        break;
    case 1000000:
        kvaserBitRate = KVASER_BITRATE_1M;
        break;
    default:
        return false;
    }

    if (updateSettingsAllowed()) {
        KvaserStatus result = canSetBusParams(m_kvaserHandle, kvaserBitRate, 0, 0, 0, 0, 0);
        if (result != KvaserStatus::OK) {
            const QString errorString = systemErrorString(result);
            setError(errorString, ConfigurationError);
            qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Failed to set bitrate: %ls", qUtf16Printable(errorString));
            return false;
        }
    }
    return true;
}

bool KvaserCanBackend::setDataBitRate(quint32 bitrate)
{
    if (canSetBusParamsFd == nullptr)
        return false;

    qint32 kvaserDataBitRate;
    switch (bitrate) {
    case 500000:
        kvaserDataBitRate = KVASER_DATA_BITRATE_500K_80P;
        break;
    case 1000000:
        kvaserDataBitRate = KVASER_DATA_BITRATE_1M_80P;
        break;
    case 2000000:
        kvaserDataBitRate = KVASER_DATA_BITRATE_2M_80P;
        break;
    case 4000000:
        kvaserDataBitRate = KVASER_DATA_BITRATE_4M_80P;
        break;
    case 8000000:
        kvaserDataBitRate = KVASER_DATA_BITRATE_8M_80P;
        break;
    default:
        return false;
    }

    if (updateSettingsAllowed()) {
        KvaserStatus result = canSetBusParamsFd(m_kvaserHandle, kvaserDataBitRate, 0, 0, 0);
        if (result != KvaserStatus::OK) {
            const QString errorString = systemErrorString(result);
            setError(errorString, ConfigurationError);
            qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Failed to set data bitrate: %ls", qUtf16Printable(errorString));
            return false;
        }
    }
    return true;
}

bool KvaserCanBackend::setCanFd(bool enable)
{
    m_canFd = enable;
    return true;
}

bool KvaserCanBackend::setFilters(const QList<Filter> &filterList)
{
    bool isStandardFrameFilterSet = false;
    bool isExtendedFrameFilterSet = false;

    if (filterList.isEmpty()) {
        if (updateSettingsAllowed()) {
            // Permit all standard frames
            KvaserStatus result = canSetAcceptanceFilter(m_kvaserHandle, 0, 0, KVASER_FILTER_STANDARD_FRAME_FORMAT);
            if (result != KvaserStatus::OK) {
                const QString errorString = systemErrorString(result);
                setError(errorString, ConfigurationError);
                qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Failed to set filters (all standard): %ls",
                          qUtf16Printable(errorString));
                return false;
            }
            // Permit all extended frames
            result = canSetAcceptanceFilter(m_kvaserHandle, 0, 0, KVASER_FILTER_EXTENDED_FRAME_FORMAT);
            if (result != KvaserStatus::OK) {
                const QString errorString = systemErrorString(result);
                setError(errorString, ConfigurationError);
                qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Failed to set filters (all extended): %ls",
                          qUtf16Printable(errorString));
                return false;
            }
        }
    } else {
        for (const Filter& filter : filterList) {
            if (filter.type != QCanBusFrame::DataFrame) {
                setError(tr("Only DataFrame filters are supported"), ConfigurationError);
                return false;
            }

            switch (filter.format) {
            case Filter::MatchBaseFormat:
            {
                if (isStandardFrameFilterSet) {
                    setError(tr("Hardware supports only one standard frame and one extended frame filter"), ConfigurationError);
                    return false;
                }
                isStandardFrameFilterSet = true;
                if (updateSettingsAllowed()) {
                    KvaserStatus result = canSetAcceptanceFilter(m_kvaserHandle, filter.frameId, filter.frameIdMask,
                                                                 KVASER_FILTER_STANDARD_FRAME_FORMAT);
                    if (result != KvaserStatus::OK) {
                        const QString errorString = systemErrorString(result);
                        setError(errorString, ConfigurationError);
                        qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Failed to set filters (standard only): %ls",
                                  qUtf16Printable(errorString));
                        return false;
                    }
                }
                break;
            }
            case Filter::MatchExtendedFormat:
            {
                if (isExtendedFrameFilterSet) {
                    setError(tr("Hardware supports only one standard frame and one extended frame filter"), ConfigurationError);
                    return false;
                }
                isExtendedFrameFilterSet = true;
                if (updateSettingsAllowed()) {
                    KvaserStatus result = canSetAcceptanceFilter(m_kvaserHandle, filter.frameId, filter.frameIdMask,
                                                                 KVASER_FILTER_EXTENDED_FRAME_FORMAT);
                    if (result != KvaserStatus::OK) {
                        const QString errorString = systemErrorString(result);
                        setError(errorString, ConfigurationError);
                        qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Failed to set filters (extended only): %ls",
                                  qUtf16Printable(errorString));
                        return false;
                    }
                }
                break;
            }
            case Filter::MatchBaseAndExtendedFormat:
            {
                if (isExtendedFrameFilterSet || isStandardFrameFilterSet) {
                    setError(tr("Hardware supports only one standard frame and one extended frame filter"), ConfigurationError);
                    return false;
                }
                isStandardFrameFilterSet = true;
                isExtendedFrameFilterSet = true;
                if (updateSettingsAllowed()) {
                    KvaserStatus result = canSetAcceptanceFilter(m_kvaserHandle, filter.frameId, filter.frameIdMask,
                                                                 KVASER_FILTER_STANDARD_FRAME_FORMAT);
                    if (result != KvaserStatus::OK) {
                        const QString errorString = systemErrorString(result);
                        setError(errorString, ConfigurationError);
                        qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Failed to set filters (standard): %ls",
                                  qUtf16Printable(errorString));
                        return false;
                    }
                    result = canSetAcceptanceFilter(m_kvaserHandle, filter.frameId, filter.frameIdMask,
                                                    KVASER_FILTER_EXTENDED_FRAME_FORMAT);
                    if (result != KvaserStatus::OK) {
                        const QString errorString = systemErrorString(result);
                        setError(errorString, ConfigurationError);
                        qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Failed to set filters (extended): %ls",
                                  qUtf16Printable(errorString));
                        return false;
                    }
                }
                break;
            }
            }
        }
    }
    return true;
}

bool KvaserCanBackend::setDriverMode(KvaserDriverMode mode)
{
    KvaserStatus result = canSetBusOutputControl(m_kvaserHandle, quint32(mode));
    if (result != KvaserStatus::OK) {
        const QString errorString = systemErrorString(result);
        setError(errorString, ConfigurationError);
        qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Failed to set driver mode: %ls",
                  qUtf16Printable(errorString));
        return false;
    }
    return true;
}

bool KvaserCanBackend::setBusOn()
{
    KvaserStatus result = canBusOn(m_kvaserHandle);
    if (result != KvaserStatus::OK) {
        const QString errorString = systemErrorString(result);
        setError(errorString, ConfigurationError);
        qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Failed to set bus on: %ls",
                  qUtf16Printable(errorString));
        return false;
    }
    return true;
}

bool KvaserCanBackend::updateSettingsAllowed()
{
    auto s = state();
    return (s == ConnectedState || s == ConnectingState) && m_initAccess;
}

QT_END_NAMESPACE
