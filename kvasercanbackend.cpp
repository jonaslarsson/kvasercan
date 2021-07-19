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
#include <QtCore/qlibrary.h>

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
        QMetaObject::invokeMethod(backend, &KvaserCanBackendPrivate::onMessagesAvailable, Qt::QueuedConnection);
    if (eventFlags & KVASER_NOTIFY_STATUS)
        QMetaObject::invokeMethod(backend, &KvaserCanBackendPrivate::onStatusChanged, Qt::QueuedConnection);
    if (eventFlags & KVASER_NOTIFY_BUSONOFF)
        QMetaObject::invokeMethod(backend, &KvaserCanBackendPrivate::onBusOnOff, Qt::QueuedConnection);
    if (eventFlags & KVASER_NOTIFY_REMOVED)
        QMetaObject::invokeMethod(backend, &KvaserCanBackendPrivate::onDeviceRemoved, Qt::QueuedConnection);
    if (eventFlags & KVASER_NOTIFY_ERROR)
        QMetaObject::invokeMethod(backend, &KvaserCanBackendPrivate::onBusError, Qt::QueuedConnection);
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

class KvaserCanWriteNotifier : public QTimer
{
    // no Q_OBJECT macro!
public:
    KvaserCanWriteNotifier(KvaserCanBackendPrivate *d, QObject *parent)
        : QTimer(parent)
        , dptr(d)
    {
        setInterval(0);
    }

protected:
    void timerEvent(QTimerEvent *e) override
    {
        if (e->timerId() == timerId()) {
            dptr->startWrite();
            return;
        }
        QTimer::timerEvent(e);
    }

private:
    KvaserCanBackendPrivate * const dptr;
};

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

    writeNotifier = new KvaserCanWriteNotifier(this, q);

    return true;
}

void KvaserCanBackendPrivate::close()
{
    if (kvaserHandle < 0)
        return;

    canClose(kvaserHandle);
    kvaserHandle = -1;

    if (writeNotifier != nullptr) {
        delete writeNotifier;
        writeNotifier = nullptr;
    }
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
    char buffer[256];
    KvaserStatus result = canGetErrorText(errorCode, buffer, sizeof(buffer));
    if (result == KvaserStatus::OK) {
        return QString::fromLatin1(buffer);
    }
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

void KvaserCanBackendPrivate::startWrite()
{
    Q_Q(KvaserCanBackend);

    if (!q->hasOutgoingFrames()) {
        writeNotifier->stop();
        return;
    }

    qint64 frameCount = 0;
    while (q->hasOutgoingFrames()) {
        const QCanBusFrame frame = q->dequeueOutgoingFrame();
        const QByteArray payload = frame.payload();

        quint32 flags = 0;
        if (frame.frameType() == QCanBusFrame::RemoteRequestFrame)
            flags |= KVASER_MESSAGE_REMOTE_REQUEST;
        else if (frame.frameType() == QCanBusFrame::ErrorFrame)
            flags |= KVASER_MESSAGE_ERROR_FRAME;
        else
            flags = 0;

        if (frame.hasExtendedFrameFormat()) {
            flags |= KVASER_MESSAGE_EXTENDED_FRAME_FORMAT;
        } else {
            flags |= KVASER_MESSAGE_STANDARD_FRAME_FORMAT;
        }

        if (kvaserHandle >= 0) {
            KvaserStatus result = canWrite(kvaserHandle, frame.frameId(), payload, payload.size(), flags);
            if (result == KvaserStatus::OK) {
                frameCount++;
            } else {
                q->setError(systemErrorString(result), QCanBusDevice::WriteError);
            }
        }
    }

    if (frameCount > 0) {
        emit q->framesWritten(frameCount);
    }
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
#if 0
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
#endif
}

void KvaserCanBackendPrivate::onBusOnOff()
{
    Q_Q(KvaserCanBackend);
    quint32 flags;
    KvaserStatus result = canReadStatus(kvaserHandle, &flags);
    if (result == KvaserStatus::OK) {
        if (flags & KVASER_STATUS_BUSOFF) {
            // TODO: Do we need to handle bus off?
        }
    } else {
        q->setError(systemErrorString(result), QCanBusDevice::ReadError);
    }
}

void KvaserCanBackendPrivate::onDeviceRemoved()
{
    Q_Q(KvaserCanBackend);
    q->close();
}

void KvaserCanBackendPrivate::onBusError()
{
    // Bus status changed Error Active, Error Passive or Error Warning
    // TODO: Do we need to handle this?
}

QCanBusDevice::CanBusStatus KvaserCanBackendPrivate::busStatus()
{
    Q_Q(KvaserCanBackend);
    if (kvaserHandle < 0) {
        return QCanBusDevice::CanBusStatus::Unknown;
    }
    quint32 flags;
    KvaserStatus result = canReadStatus(kvaserHandle, &flags);
    if (result != KvaserStatus::OK) {
        const QString errorString = systemErrorString(result);
        qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Can not query CAN bus status: %ls.",
            qUtf16Printable(errorString));
        q->setError(errorString, QCanBusDevice::CanBusError::ReadError);
        return QCanBusDevice::CanBusStatus::Unknown;
    }
    if (flags & KVASER_STATUS_BUSOFF) {
        return QCanBusDevice::CanBusStatus::BusOff;
    } else if (flags & KVASER_STATUS_ERROR_PASSIVE) {
        return QCanBusDevice::CanBusStatus::Error;
    } else if (flags & KVASER_STATUS_ERROR_WARNING) {
        return QCanBusDevice::CanBusStatus::Warning;
    } else if (flags & KVASER_STATUS_ERROR_ACTIVE) {
        return QCanBusDevice::CanBusStatus::Good;
    }
    qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Unknown CAN bus status flags: 0x%08x", flags);
    return QCanBusDevice::CanBusStatus::Unknown;
}

void KvaserCanBackendPrivate::resetController()
{
    Q_Q(KvaserCanBackend);
    if (kvaserHandle < 0)
        return;
    KvaserStatus result = canResetBus(kvaserHandle);
    if (result != KvaserStatus::OK) {
        const QString errorString = systemErrorString(result);
        qCWarning(QT_CANBUS_PLUGINS_KVASERCAN, "Failed to reset can bus: %ls.",
            qUtf16Printable(errorString));
        q->setError(errorString, QCanBusDevice::CanBusError::ReadError);
    }
}

KvaserCanBackend::KvaserCanBackend(const QString &name, QObject *parent)
    : QCanBusDevice(parent)
    , d_ptr(new KvaserCanBackendPrivate(this))
{
    Q_D(KvaserCanBackend);
    d->setupChannel(name);
    d->setupDefaultConfigurations();

    std::function<void()> f = std::bind(&KvaserCanBackendPrivate::resetController, d_ptr);
    setResetControllerFunction(f);

    std::function<CanBusStatus()> g = std::bind(&KvaserCanBackendPrivate::busStatus, d_ptr);
    setCanBusStatusGetter(g);
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
    Q_D(KvaserCanBackend);
    if (d->setConfigurationParameter(key, value))
        QCanBusDevice::setConfigurationParameter(key, value);
}

bool KvaserCanBackend::writeFrame(const QCanBusFrame &newData)
{
    Q_D(KvaserCanBackend);

    if (state() != QCanBusDevice::ConnectedState)
        return false;

    if (Q_UNLIKELY(!newData.isValid())) {
        setError(tr("Cannot write invalid QCanBusFrame"), QCanBusDevice::WriteError);
        return false;
    }

    if (Q_UNLIKELY(newData.frameType() != QCanBusFrame::DataFrame
            && newData.frameType() != QCanBusFrame::RemoteRequestFrame
            && newData.frameType() != QCanBusFrame::ErrorFrame)) {
        setError(tr("Unable to write a frame with unacceptable type"),
                 QCanBusDevice::WriteError);
        return false;
    }

    enqueueOutgoingFrame(newData);

    if (!d->writeNotifier->isActive())
        d->writeNotifier->start();

    return true;
}

QString KvaserCanBackend::interpretErrorFrame(const QCanBusFrame &errorFrame)
{
    if (errorFrame.frameType() != QCanBusFrame::ErrorFrame)
        return QString();
    // TODO: Implement this
    return QString();
}

QT_END_NAMESPACE
