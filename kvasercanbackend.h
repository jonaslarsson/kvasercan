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

#ifndef KVASERCANBACKEND_H
#define KVASERCANBACKEND_H

#include "kvasercan_symbols_p.h"

#include <QtSerialBus/qcanbusframe.h>
#include <QtSerialBus/qcanbusdevice.h>
#include <QtSerialBus/qcanbusdeviceinfo.h>

#include <QtCore/qlist.h>
#include <QtCore/qvariant.h>

QT_BEGIN_NAMESPACE

class QTimer;
class KvaserCanBackend : public QCanBusDevice
{
    Q_OBJECT
    Q_DISABLE_COPY(KvaserCanBackend)

public:
    explicit KvaserCanBackend(const QString &name, QObject *parent = nullptr);
    ~KvaserCanBackend();
    bool open() override;
    void close() override;
    void setConfigurationParameter(ConfigurationKey key, const QVariant &value) override;
    bool writeFrame(const QCanBusFrame &frame) override;
    QString interpretErrorFrame(const QCanBusFrame &errorFrame) override;
    static bool canCreate(QString *errorReason);
    static QList<QCanBusDeviceInfo> interfaces();
    QCanBusDevice::CanBusStatus busStatus() override;
    void resetController() override;
    void setMessagesAvailable()
    {
        if (m_messagesAvailable == false) {
            m_messagesAvailable = true;
            QMetaObject::invokeMethod(this, &KvaserCanBackend::onMessagesAvailable, Qt::QueuedConnection);
        }
    }

public slots:
    void onMessagesAvailable();
    void onStatusChanged();
    void onBusOnOff();
    void onDeviceRemoved();

private:
    bool applyConfigurationParameter(ConfigurationKey key, const QVariant &value);
    void setupChannel(const QString& interfaceName);
    void setupDefaultConfigurations();
    bool setReceiveOwnKey(bool enable);
    bool setLoopback(bool enable);
    bool setBitRate(quint32 bitrate);
    bool setDataBitRate(quint32 bitrate);
    bool setCanFd(bool enable);
    bool setFilters(const QList<QCanBusDevice::Filter>& filterList);
    bool setDriverMode(KvaserDriverMode mode);
    bool setBusOn();
    bool updateSettingsAllowed();
    QString m_interfaceName;
    KvaserHandle m_kvaserHandle = -1;
    bool m_initAccess = true;
    bool m_messagesAvailable = false;
    bool m_canFd = false;
};

QT_END_NAMESPACE

#endif // KVASERCANBACKEND_H
