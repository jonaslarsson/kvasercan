/****************************************************************************
**
** Copyright (C) 2017 Denis Shienkov <denis.shienkov@gmail.com>
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

#ifndef KVASERCANBACKEND_P_H
#define KVASERCANBACKEND_P_H

#include "kvasercan_symbols_p.h"
#include "kvasercanbackend.h"

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

QT_BEGIN_NAMESPACE

class KvaserCanBackendPrivate
{
    Q_DECLARE_PUBLIC(KvaserCanBackend)
public:
    KvaserCanBackendPrivate(KvaserCanBackend *q);
    ~KvaserCanBackendPrivate();

    bool open();
    void close();
    bool setConfigurationParameter(QCanBusDevice::ConfigurationKey key, const QVariant& value);
    void setupChannel(const QString& interfaceName);
    void setupDefaultConfigurations();
    QString systemErrorString(KvaserStatus errorCode) const;
    bool setBitRate(quint32 bitrate);
    bool setDriverMode(KvaserDriverMode mode);
    bool setBusOn();

    void onMessagesAvailable();
    void onStatusChanged();
    void onBusOnOff();
    void onDeviceRemoved();
    void onBusError();

    KvaserCanBackend * const q_ptr;

    int bitRate;
    int channelIndex = -1;
    KvaserHandle kvaserHandle = -1;
    bool initAccess;
};

QT_END_NAMESPACE

#endif // KVASERCANBACKEND_P_H
