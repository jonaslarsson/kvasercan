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

#ifndef KVASERCAN_SYMBOLS_P_H
#define KVASERCAN_SYMBOLS_P_H

#include <QtCore/qlibrary.h>
#include <QtCore/qsettings.h>

#ifdef Q_OS_WIN32
#  include <windows.h>
#else
#  error "Unsupported platform"
#endif

#define GENERATE_SYMBOL_VARIABLE(returnType, symbolName, ...) \
    typedef returnType (WINAPI *fp_##symbolName)(__VA_ARGS__); \
    static fp_##symbolName symbolName;

#define RESOLVE_SYMBOL(symbolName) \
    symbolName = reinterpret_cast<fp_##symbolName>(kvasercanLibrary->resolve(#symbolName)); \
    if (!symbolName) \
        return false;

enum class KvaserStatus {
    OK = 0,
    NoMessages = -2
};

enum class CanChannelDataItem {
    Capabilities = 1,
    CardChannelNumber = 6,
    CardSerialNumber = 7,
    DeviceProductName  = 26
};

enum class KvaserDriverMode {
    Silent = 1,
    Normal = 4
};

#define KVASER_CAPABILITY_VIRTUAL          0x10000
#define KVASER_CAPABILITY_CANFD            0x80000
#define KVASER_CAPABILITY_CANFD_NON_ISO   0x100000

#define KVASER_BITRATE_10K  (-9)
#define KVASER_BITRATE_50K  (-7)
#define KVASER_BITRATE_62K  (-6)
#define KVASER_BITRATE_83K  (-8)
#define KVASER_BITRATE_100K (-5)
#define KVASER_BITRATE_125K (-4)
#define KVASER_BITRATE_250K (-3)
#define KVASER_BITRATE_500K (-2)
#define KVASER_BITRATE_1M   (-1)

#define KVASER_NOTIFY_RX        0x01
#define KVASER_NOTIFY_TX        0x02
#define KVASER_NOTIFY_ERROR     0x04
#define KVASER_NOTIFY_STATUS    0x08
#define KVASER_NOTIFY_BUSONOFF  0x20
#define KVASER_NOTIFY_REMOVED   0x40

#define KVASER_STATUS_ERROR_PASSIVE    0x1
#define KVASER_STATUS_BUSOFF           0x2
#define KVASER_STATUS_ERROR_WARNING    0x4
#define KVASER_STATUS_ERROR_ACTIVE     0x8
#define KVASER_STATUS_TX_PENDING      0x10
#define KVASER_STATUS_RX_PENDING      0x20
#define KVASER_STATUS_TX_ERROR        0x80
#define KVASER_STATUS_RX_ERROR       0x100
#define KVASER_STATUS_HW_OVERRUN     0x200
#define KVASER_STATUS_SW_OVERRUN     0x400

#define KVASER_MESSAGE_REMOTE_REQUEST            0x1
#define KVASER_MESSAGE_STANDARD_FRAME_FORMAT     0x2
#define KVASER_MESSAGE_EXTENDED_FRAME_FORMAT     0x4
#define KVASER_MESSAGE_ERROR_FRAME              0x20

#define KVASER_OPEN_ACCEPT_VIRTUAL       0x20
#define KVASER_OPEN_REQUIRE_INIT_ACCESS  0x80
#define KVASER_OPEN_NO_INIT_ACCESS      0x100

typedef int KvaserHandle;
typedef void (WINAPI *KvaserCallback) (KvaserHandle, void *, quint32);

GENERATE_SYMBOL_VARIABLE(void, canInitializeLibrary, void)
GENERATE_SYMBOL_VARIABLE(KvaserStatus, canGetNumberOfChannels, int *)
GENERATE_SYMBOL_VARIABLE(KvaserStatus, canGetChannelData, int, CanChannelDataItem, void *, size_t)
GENERATE_SYMBOL_VARIABLE(KvaserStatus, canIoCtl, KvaserHandle, quint32, void *, quint32)
GENERATE_SYMBOL_VARIABLE(KvaserHandle, canOpenChannel, int, int)
GENERATE_SYMBOL_VARIABLE(KvaserStatus, canClose, KvaserHandle)
GENERATE_SYMBOL_VARIABLE(KvaserStatus, canSetBusParams, KvaserHandle, qint32, quint32, quint32, quint32, quint32, quint32)
GENERATE_SYMBOL_VARIABLE(KvaserStatus, canSetBusOutputControl, KvaserHandle, quint32)
GENERATE_SYMBOL_VARIABLE(KvaserStatus, canBusOn, KvaserHandle)
GENERATE_SYMBOL_VARIABLE(KvaserStatus, canBusOff, KvaserHandle)
GENERATE_SYMBOL_VARIABLE(KvaserStatus, kvSetNotifyCallback, KvaserHandle, KvaserCallback, void *, quint32)
GENERATE_SYMBOL_VARIABLE(KvaserStatus, canReadStatus, KvaserHandle, quint32 * const)
GENERATE_SYMBOL_VARIABLE(KvaserStatus, canRead, KvaserHandle, quint32 *, void *, quint32 *, quint32 *, quint32 *)
GENERATE_SYMBOL_VARIABLE(KvaserStatus, canGetErrorText, KvaserStatus, char *, size_t)
GENERATE_SYMBOL_VARIABLE(KvaserStatus, canResetBus , KvaserHandle)
GENERATE_SYMBOL_VARIABLE(KvaserStatus, canWrite , KvaserHandle, quint32, const void *, quint32, quint32)

inline bool resolveKvaserCanSymbols(QLibrary *kvasercanLibrary)
{
    if (!kvasercanLibrary->isLoaded()) {
        kvasercanLibrary->setFileName(QStringLiteral("canlib32"));
#ifdef Q_OS_WIN32
         if (!kvasercanLibrary->load()) {
            kvasercanLibrary->unload();
            // Look for Kvaser library installation path to locate canlib32.dll
            QSettings settings("HKEY_LOCAL_MACHINE\\SOFTWARE\\KVASER AB\\CANLIB32", QSettings::NativeFormat);
            if (settings.contains("InstallDir")) {
                QString installDir = settings.value("InstallDir").toString();
                kvasercanLibrary->setFileName(installDir + "\\canlib32.dll");
            }
        }
#endif
        if (!kvasercanLibrary->load())
            return false;
    }

    RESOLVE_SYMBOL(canInitializeLibrary)
    RESOLVE_SYMBOL(canGetNumberOfChannels)
    RESOLVE_SYMBOL(canGetChannelData)
    RESOLVE_SYMBOL(canIoCtl)
    RESOLVE_SYMBOL(canOpenChannel)
    RESOLVE_SYMBOL(canClose)
    RESOLVE_SYMBOL(canSetBusParams)
    RESOLVE_SYMBOL(canSetBusOutputControl)
    RESOLVE_SYMBOL(canBusOn)
    RESOLVE_SYMBOL(canBusOff)
    RESOLVE_SYMBOL(kvSetNotifyCallback)
    RESOLVE_SYMBOL(canReadStatus)
    RESOLVE_SYMBOL(canRead)
    RESOLVE_SYMBOL(canGetErrorText)
    RESOLVE_SYMBOL(canResetBus)
    RESOLVE_SYMBOL(canWrite)
    return true;
}

#endif // KVASERCAN_SYMBOLS_P_H
