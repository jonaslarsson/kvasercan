/****************************************************************************
**
** Copyright (C) 2017 Denis Shienkov <denis.shienkov@gmail.com>
** Copyright (C) 2019 Andre Hartmann <aha_1980@gmx.de>
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
#include "kvasercanbackend_p.h"
#include "kvasercan_symbols_p.h"

#include <QtSerialBus/qcanbusdevice.h>

#include <QtCore/qcoreevent.h>
#include <QtCore/qloggingcategory.h>
#include <QtCore/qtimer.h>

#include <QSettings>
#include <QLibrary>
#include <QFileInfo>

#include <algorithm>

QT_BEGIN_NAMESPACE

Q_DECLARE_LOGGING_CATEGORY(QT_CANBUS_PLUGINS_KVASERCAN)

#ifndef LINK_LIBKVASERCAN
Q_GLOBAL_STATIC(QLibrary, kvasercanLibrary)
#endif

static void WINAPI callbackHandler(KvaserHandle, void *internalPointer, quint32 eventFlags)
{
    KvaserCanBackendPrivate *backend = static_cast<KvaserCanBackendPrivate*>(internalPointer);
    if (eventFlags & KVASER_NOTIFY_RX)
        backend->onMessagesAvailable();
    if (eventFlags & KVASER_NOTIFY_STATUS)
        backend->onStatusChanged();
    if (eventFlags & KVASER_NOTIFY_BUSONOFF)
        backend->onBusOnOff();
    if (eventFlags & KVASER_NOTIFY_REMOVED)
        backend->onDeviceRemoved();
    if (eventFlags & KVASER_NOTIFY_ERROR)
        backend->onBusError();
}

bool KvaserCanBackend::canCreate(QString *errorReason)
{
#ifdef LINK_LIBKVASERCAN
    return true;
#else
    static bool symbolsResolved = resolveKvaserCanSymbols(kvasercanLibrary());
    if (Q_UNLIKELY(!symbolsResolved)) {
        *errorReason = kvasercanLibrary()->errorString();
        return false;
    }
    return true;
#endif
}

QList<QCanBusDeviceInfo> KvaserCanBackend::interfaces()
{
    QList<QCanBusDeviceInfo> result;

    canInitializeLibrary();

    int channelCount = 0;
    if (canGetNumberOfChannels(&channelCount) != KvaserStatus::OK) {
        qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Cannot get number of channels");
        channelCount = 0;
    }

    for (int channel = 0; channel < channelCount; ++channel) {
        char name[256];
        if (canGetChannelData(channel, CanChannelDataItem::DeviceProductName, &name, sizeof(name)) != KvaserStatus::OK)
            continue;

        quint64 serial;
        if (canGetChannelData(channel, CanChannelDataItem::CardSerialNumber, &serial, sizeof(serial)) != KvaserStatus::OK)
            continue;

        quint32 channelOnCard = 0;
        if (canGetChannelData(channel, CanChannelDataItem::CardChannelNumber, &channelOnCard, sizeof(channelOnCard)) != KvaserStatus::OK)
            continue;

        quint32 capabilities = 0;
        if (canGetChannelData(channel, CanChannelDataItem::Capabilities, &capabilities, sizeof(capabilities)) != KvaserStatus::OK)
            continue;

        const bool isVirtual = capabilities & KVASER_CAPABILITY_VIRTUAL;

        // Currently no support for CAN FD devices
        const bool isCanFd = false;
        if (capabilities & KVASER_CAPABILITY_CANFD)
            continue;
        if (capabilities & KVASER_CAPABILITY_CANFD_NON_ISO)
            continue;

        QCanBusDeviceInfo info = createDeviceInfo(QString::fromLatin1("can%1").arg(channel),
                                                  QString::number(serial),
                                                  QLatin1String(name),
                                                  int(channelOnCard),
                                                  isVirtual, isCanFd);
        result.append(std::move(info));
    }

    return result;
}

KvaserCanBackendPrivate::KvaserCanBackendPrivate(KvaserCanBackend *q)
    : q_ptr(q)
{
}

KvaserCanBackendPrivate::~KvaserCanBackendPrivate()
{
}

bool KvaserCanBackendPrivate::open()
{
    Q_Q(KvaserCanBackend);

    if (channelIndex < 0) {
        q->setError(KvaserCanBackend::tr("Invalid channel"),
                    QCanBusDevice::CanBusError::ConnectionError);
        return false;
    }

    initAccess = true;
    kvaserHandle = canOpenChannel(channelIndex, KVASER_OPEN_ACCEPT_VIRTUAL | KVASER_OPEN_REQUIRE_INIT_ACCESS);

    if (kvaserHandle < 0) {
        initAccess = false;
        kvaserHandle = canOpenChannel(channelIndex, KVASER_OPEN_ACCEPT_VIRTUAL | KVASER_OPEN_NO_INIT_ACCESS);
    }

    if (kvaserHandle < 0) {
        q->setError(systemErrorString((KvaserStatus)kvaserHandle), QCanBusDevice::CanBusError::ConnectionError);
        return false;
    } else {
        KvaserStatus result = kvSetNotifyCallback(kvaserHandle, callbackHandler, this, KVASER_NOTIFY_RX | KVASER_NOTIFY_BUSONOFF |
                                                  KVASER_NOTIFY_ERROR | KVASER_NOTIFY_REMOVED | KVASER_NOTIFY_STATUS);
        if (result != KvaserStatus::OK) {
            q->setError(systemErrorString(result), QCanBusDevice::CanBusError::ConnectionError);
            return false;
        }
    }

    return true;
}

void KvaserCanBackendPrivate::close()
{
    if (kvaserHandle < 0)
        return;
    canClose(kvaserHandle);
    kvaserHandle = -1;
}

bool KvaserCanBackendPrivate::setConfigurationParameter(QCanBusDevice::ConfigurationKey key, const QVariant &value)
{
    Q_Q(KvaserCanBackend);

    switch (key) {
    case QCanBusDevice::BitRateKey:
        return setBitRate(value.toUInt());
    default:
        q->setError(KvaserCanBackend::tr("Unsupported configuration key: %1").arg(key),
                    QCanBusDevice::ConfigurationError);
        return false;
    }
}

void KvaserCanBackendPrivate::setupChannel(const QString &interfaceName)
{
    Q_Q(KvaserCanBackend);
    if (Q_LIKELY(interfaceName.startsWith(QStringLiteral("can")))) {
        const QStringView ref = QStringView{interfaceName}.mid(3);
        bool ok = false;
        channelIndex = ref.toInt(&ok);
        if (ok && channelIndex >= 0) {
            return;
        } else {
            channelIndex = -1;
            q->setError(KvaserCanBackend::tr("Unable to setup channel with interface name %1")
                            .arg(interfaceName), QCanBusDevice::CanBusError::ConfigurationError);
        }
    }

    qCCritical(QT_CANBUS_PLUGINS_KVASERCAN, "Unable to parse the channel %ls",
               qUtf16Printable(interfaceName));
}

void KvaserCanBackendPrivate::setupDefaultConfigurations()
{
    setConfigurationParameter(QCanBusDevice::BitRateKey, 500000);
}

QString KvaserCanBackendPrivate::systemErrorString(KvaserStatus errorCode) const
{
    // TODO: Get error string
    return KvaserCanBackend::tr("Unable to retrieve an error string");
}

bool KvaserCanBackendPrivate::setBitRate(quint32 bitrate)
{
    Q_Q(KvaserCanBackend);
    switch (bitrate) {
    case 10000:
        bitRate = KVASER_BITRATE_10K;
        break;
    case 50000:
        bitRate = KVASER_BITRATE_50K;
        break;
    case 62000:
        bitRate = KVASER_BITRATE_62K;
        break;
    case 83000:
        bitRate = KVASER_BITRATE_83K;
        break;
    case 100000:
        bitRate = KVASER_BITRATE_100K;
        break;
    case 125000:
        bitRate = KVASER_BITRATE_125K;
        break;
    case 250000:
        bitRate = KVASER_BITRATE_250K;
        break;
    case 500000:
        bitRate = KVASER_BITRATE_500K;
        break;
    case 1000000:
        bitRate = KVASER_BITRATE_1M;
        break;
    default:
        return false;
    }

    if (q->state() == QCanBusDevice::ConnectedState && initAccess) {
        KvaserStatus result = canSetBusParams(kvaserHandle, bitRate, 0, 0, 0, 0, 0);
        if (result != KvaserStatus::OK) {
            q->setError(systemErrorString(result), QCanBusDevice::ConfigurationError);
            return false;
        }
    }
    return true;
}

bool KvaserCanBackendPrivate::setDriverMode(KvaserDriverMode mode)
{
    Q_Q(KvaserCanBackend);
    KvaserStatus result = canSetBusOutputControl(kvaserHandle, (quint32)mode);
    if (result != KvaserStatus::OK) {
        q->setError(systemErrorString(result), QCanBusDevice::ConfigurationError);
        return false;
    }
    return true;
}

bool KvaserCanBackendPrivate::setBusOn()
{
    Q_Q(KvaserCanBackend);
    KvaserStatus result = canBusOn(kvaserHandle);
    if (result != KvaserStatus::OK) {
        q->setError(systemErrorString(result), QCanBusDevice::ConfigurationError);
        return false;
    }
    return true;
}

void KvaserCanBackendPrivate::onMessagesAvailable()
{
    Q_Q(KvaserCanBackend);

    QList<QCanBusFrame> newFrames;
    quint32 frameId;
    char buffer[64];
    quint32 dlc;
    quint32 flags;
    quint32 time;

    KvaserStatus result = canRead(kvaserHandle, &frameId, buffer, &dlc, &flags, &time);
    while (result == KvaserStatus::OK) {
        QCanBusFrame frame;
        frame.setTimeStamp(QCanBusFrame::TimeStamp::fromMicroSeconds((qint64)time * 1000));
        frame.setFrameType(QCanBusFrame::DataFrame);
        if (flags & KVASER_MESSAGE_REMOTE_REQUEST)
            frame.setFrameType(QCanBusFrame::RemoteRequestFrame);
        if (flags & KVASER_MESSAGE_ERROR_FRAME)
            frame.setFrameType(QCanBusFrame::ErrorFrame);
        frame.setExtendedFrameFormat(flags & KVASER_MESSAGE_EXTENDED_FRAME_FORMAT);
        frame.setFrameId(frameId);
        frame.setPayload(QByteArray(buffer, dlc));
        newFrames.append(frame);
        result = canRead(kvaserHandle, &frameId, buffer, &dlc, &flags, &time);
    }

    q->enqueueReceivedFrames(newFrames);

    if (result != KvaserStatus::NoMessages) {
        q->setError(systemErrorString(result), QCanBusDevice::ReadError);
    }
}

void KvaserCanBackendPrivate::onStatusChanged()
{
    Q_Q(KvaserCanBackend);
    quint32 flags;
    KvaserStatus result = canReadStatus(kvaserHandle, &flags);
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
        q->setError(systemErrorString(result), QCanBusDevice::ConnectionError);
    }
}

void KvaserCanBackendPrivate::onBusOnOff()
{
    Q_Q(KvaserCanBackend);
    quint32 flags;
    KvaserStatus result = canReadStatus(kvaserHandle, &flags);
    if (result == KvaserStatus::OK) {
        if (flags & KVASER_STATUS_BUSOFF) {
            qDebug() << Q_FUNC_INFO << "OFF";
            // TODO: Handle bus off
        } else {
            qDebug() << Q_FUNC_INFO << "ON";
        }
    } else {
        q->setError(systemErrorString(result), QCanBusDevice::ConnectionError);
    }
}

void KvaserCanBackendPrivate::onDeviceRemoved()
{
    qDebug() << Q_FUNC_INFO;
    // TODO: Handle device removed
}

void KvaserCanBackendPrivate::onBusError()
{
    qDebug() << Q_FUNC_INFO;
    // TODO: Handle bus error
}

KvaserCanBackend::KvaserCanBackend(const QString &name, QObject *parent)
    : QCanBusDevice(parent)
    , d_ptr(new KvaserCanBackendPrivate(this))
{
    Q_D(KvaserCanBackend);
    d->setupChannel(name);
    d->setupDefaultConfigurations();
}

KvaserCanBackend::~KvaserCanBackend()
{
    if (state() == ConnectedState)
        KvaserCanBackend::close();
    delete d_ptr;
}

bool KvaserCanBackend::open()
{
    Q_D(KvaserCanBackend);

    if (!d->open()) {
        close(); // sets UnconnectedState
        return false;
    }

    setState(QCanBusDevice::ConnectedState);

    const auto keys = configurationKeys();
    for (ConfigurationKey key : keys) {
        const QVariant param = configurationParameter(key);
        const bool success = d->setConfigurationParameter(key, param);
        if (!success) {
            qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Cannot apply parameter: %d with value: %ls.",
                      key, qUtf16Printable(param.toString()));
        }
    }

    if (!d->setDriverMode(KvaserDriverMode::Normal)) {
        close();
        return false;
    }

    if (!d->setBusOn()) {
        close();
        return false;
    }

    return true;
}

void KvaserCanBackend::close()
{
    Q_D(KvaserCanBackend);

    d->close();

    setState(QCanBusDevice::UnconnectedState);
}

void KvaserCanBackend::setConfigurationParameter(ConfigurationKey key, const QVariant &value)
{
    qDebug() << Q_FUNC_INFO << key << value;
    Q_D(KvaserCanBackend);

    if (d->setConfigurationParameter(key, value))
        QCanBusDevice::setConfigurationParameter(key, value);
}

bool KvaserCanBackend::writeFrame(const QCanBusFrame &newData)
{
    // TODO: Implement this
    return false;
}

QString KvaserCanBackend::interpretErrorFrame(const QCanBusFrame &errorFrame)
{
    // TODO: Implement this
    return QString();
}

QT_END_NAMESPACE
