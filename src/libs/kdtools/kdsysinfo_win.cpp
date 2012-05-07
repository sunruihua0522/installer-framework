/****************************************************************************
** Copyright (C) 2001-2010 Klaralvdalens Datakonsult AB.  All rights reserved.
**
** This file is part of the KD Tools library.
**
** Licensees holding valid commercial KD Tools licenses may use this file in
** accordance with the KD Tools Commercial License Agreement provided with
** the Software.
**
**
** This file may be distributed and/or modified under the terms of the
** GNU Lesser General Public License version 2 and version 3 as published by the
** Free Software Foundation and appearing in the file LICENSE.LGPL included.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
** Contact info@kdab.com if any conditions of this licensing are not
** clear to you.
**
**********************************************************************/

#include "kdsysinfo.h"

#include <windows.h>
#include <Psapi.h>
#include <Tlhelp32.h>

#include <Winnetwk.h>
#pragma comment(lib, "mpr.lib")

#include <QDebug>
#include <QDir>
#include <QLibrary>

const int KDSYSINFO_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;

namespace KDUpdater {

quint64 installedMemory()
{
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    GlobalMemoryStatusEx(&status);
    return quint64(status.ullTotalPhys);
}

VolumeInfo updateVolumeSizeInformation(const VolumeInfo &info)
{
    ULARGE_INTEGER bytesTotal;
    ULARGE_INTEGER freeBytesPerUser;

    VolumeInfo update = info;
    if (GetDiskFreeSpaceExA(qPrintable(info.volumeDescriptor()), &freeBytesPerUser, &bytesTotal, NULL)) {
        update.setSize(bytesTotal.QuadPart);
        update.setAvailableSize(freeBytesPerUser.QuadPart);
    }
    return update;
}

/*!
    Returns a list of volume info objects that are mounted as network drive shares.
*/
QList<VolumeInfo> networkVolumeInfosFromMountPoints()
{
    QList<VolumeInfo> volumes;
    QFileInfoList drives = QDir::drives();
    foreach (const QFileInfo &drive, drives) {
        const QString driveLetter = QDir::toNativeSeparators(drive.canonicalPath());
        const uint driveType = GetDriveTypeA(qPrintable(driveLetter));
        switch (driveType) {
            case DRIVE_REMOTE: {
                char buffer[1024] = "";
                DWORD bufferLength = 1024;
                UNIVERSAL_NAME_INFOA *universalNameInfo = (UNIVERSAL_NAME_INFOA*) &buffer;
                if (WNetGetUniversalNameA(qPrintable(driveLetter), UNIVERSAL_NAME_INFO_LEVEL,
                    LPVOID(universalNameInfo), &bufferLength) == NO_ERROR) {
                        VolumeInfo info;
                        info.setMountPath(driveLetter);
                        info.setVolumeDescriptor(QLatin1String(universalNameInfo->lpUniversalName));
                        volumes.append(info);
                }
            }   break;

            default:
                break;
        }
    }
    return volumes;
}

/*!
    Returns a list of volume info objects based on the given \a volumeGUID. The function also solves mounted
    volume folder paths. It does not return any network drive shares.
*/
QList<VolumeInfo> localVolumeInfosFromMountPoints(const QByteArray &volumeGUID)
{
    QList<VolumeInfo> volumes;
    DWORD bufferSize;
    char volumeNames[1024] = "";
    if (GetVolumePathNamesForVolumeNameA(volumeGUID, volumeNames, ARRAYSIZE(volumeNames), &bufferSize)) {
        QStringList mountedPaths = QString::fromLatin1(volumeNames, bufferSize).split(QLatin1Char(char(0)),
            QString::SkipEmptyParts);
        foreach (const QString &mountedPath, mountedPaths) {
            VolumeInfo info;
            info.setMountPath(mountedPath);
            info.setVolumeDescriptor(QString::fromLatin1(volumeGUID));
            volumes.append(info);
        }
    }
    return volumes;
}

QList<VolumeInfo> mountedVolumes()
{
    QList<VolumeInfo> tmp;
    char volumeGUID[MAX_PATH] = "";
    HANDLE handle = FindFirstVolumeA(volumeGUID, ARRAYSIZE(volumeGUID));
    if (handle != INVALID_HANDLE_VALUE) {
        tmp += localVolumeInfosFromMountPoints(volumeGUID);
        while (FindNextVolumeA(handle, volumeGUID, ARRAYSIZE(volumeGUID))) {
            tmp += localVolumeInfosFromMountPoints(volumeGUID);
        }
        FindVolumeClose(handle);
    }
    tmp += networkVolumeInfosFromMountPoints();

    QList<VolumeInfo> volumes;
    while (!tmp.isEmpty()) // update volume size information
        volumes.append(updateVolumeSizeInformation(tmp.takeFirst()));
    return volumes;
}

struct EnumWindowsProcParam
{
    QList<ProcessInfo> processes;
    QList<quint32> seenIDs;
};

typedef BOOL (WINAPI *QueryFullProcessImageNamePtr)(HANDLE, DWORD, char *, PDWORD);
typedef DWORD (WINAPI *GetProcessImageFileNamePtr)(HANDLE, char *, DWORD);

QList<ProcessInfo> runningProcesses()
{
    EnumWindowsProcParam param;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (!snapshot)
        return param.processes;
    PROCESSENTRY32 processStruct;
    processStruct.dwSize = sizeof(PROCESSENTRY32);
    bool foundProcess = Process32First(snapshot, &processStruct);
    const DWORD bufferSize = 1024;
    char driveBuffer[bufferSize];
    QStringList deviceList;
    if (QSysInfo::windowsVersion() <= QSysInfo::WV_5_2) {
        DWORD size = GetLogicalDriveStringsA(bufferSize, driveBuffer);
        deviceList = QString::fromLatin1(driveBuffer, size).split(QLatin1Char(char(0)), QString::SkipEmptyParts);
    }

    QLibrary kernel32(QLatin1String("Kernel32.dll"));
    kernel32.load();
    void *pQueryFullProcessImageNameA = kernel32.resolve("QueryFullProcessImageNameA");

    QLibrary psapi(QLatin1String("Psapi.dll"));
    psapi.load();
    void *pGetProcessImageFileNamePtr = psapi.resolve("GetProcessImageFileNameA");
    QueryFullProcessImageNamePtr callPtr = (QueryFullProcessImageNamePtr) pQueryFullProcessImageNameA;
    GetProcessImageFileNamePtr callPtrXp = (GetProcessImageFileNamePtr) pGetProcessImageFileNamePtr;

    while (foundProcess) {
        HANDLE procHandle = OpenProcess(QSysInfo::windowsVersion() > QSysInfo::WV_5_2
                                            ? KDSYSINFO_PROCESS_QUERY_LIMITED_INFORMATION
                                            : PROCESS_QUERY_INFORMATION,
                                         false,
                                         processStruct.th32ProcessID);
        char buffer[1024];
        DWORD bufferSize = 1024;
        bool succ = false;
        QString executablePath;
        ProcessInfo info;

        if (QSysInfo::windowsVersion() > QSysInfo::WV_5_2) {
            succ = callPtr(procHandle, 0, buffer, &bufferSize);
            executablePath = QString::fromLatin1(buffer);
        } else if (pGetProcessImageFileNamePtr) {
            succ = callPtrXp(procHandle, buffer, bufferSize);
            executablePath = QString::fromLatin1(buffer);
            for (int i = 0; i < deviceList.count(); ++i) {
                executablePath.replace(QString::fromLatin1( "\\Device\\HarddiskVolume%1\\" ).arg(i + 1),
                    deviceList.at(i));
            }
        }

        if (succ) {
            const quint32 pid = processStruct.th32ProcessID;
            param.seenIDs.append(pid);
            info.id = pid;
            info.name = executablePath;
            param.processes.append(info);
        }

        CloseHandle(procHandle);
        foundProcess = Process32Next(snapshot, &processStruct);

    }
    if (snapshot)
        CloseHandle(snapshot);

    kernel32.unload();
    return param.processes;
}

bool CALLBACK TerminateAppEnum(HWND hwnd, LPARAM lParam)
{
   DWORD dwID;
   GetWindowThreadProcessId(hwnd, &dwID);

   if (dwID == (DWORD)lParam)
      PostMessage(hwnd, WM_CLOSE, 0, 0);

   return true;
}

bool killProcess(const ProcessInfo &process, int msecs)
{
    DWORD dwTimeout = msecs;
    if (msecs == -1)
        dwTimeout = INFINITE;

    // If we can't open the process with PROCESS_TERMINATE rights,
    // then we give up immediately.
    HANDLE hProc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, process.id);

    if (hProc == 0)
        return false;

    // TerminateAppEnum() posts WM_CLOSE to all windows whose PID
    // matches your process's.
    EnumWindows((WNDENUMPROC)TerminateAppEnum, (LPARAM)process.id);

    // Wait on the handle. If it signals, great. If it times out,
    // then you kill it.
    bool returnValue = false;
    if (WaitForSingleObject(hProc, dwTimeout) != WAIT_OBJECT_0)
        returnValue = TerminateProcess(hProc, 0);

    CloseHandle(hProc) ;

    return returnValue;
}

// REPARSE_DATA_BUFFER structure from msdn help: http://msdn.microsoft.com/en-us/library/ff552012.aspx
typedef struct _REPARSE_DATA_BUFFER {
    ULONG  ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    union {
        struct {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            ULONG  Flags;
            WCHAR  PathBuffer[1];
        } SymbolicLinkReparseBuffer;
        struct {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            WCHAR  PathBuffer[1];
        } MountPointReparseBuffer;
        struct {
            UCHAR DataBuffer[1];
        } GenericReparseBuffer;
    };
} REPARSE_DATA_BUFFER, *PREPARSE_DATA_BUFFER;

QString junctionTargetPath(const QString &path)
{
    HANDLE fileHandle;
    fileHandle = CreateFile(path.utf16(), FILE_READ_EA, FILE_SHARE_READ | FILE_SHARE_WRITE |
                            FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
                            FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (fileHandle == INVALID_HANDLE_VALUE) {
        qDebug() << QString::fromLatin1("Could not open: '%1'; error: %2\n").arg(path).arg(GetLastError());
        return path;
    }

    REPARSE_DATA_BUFFER* reparseStructData = (REPARSE_DATA_BUFFER*)calloc(1, MAXIMUM_REPARSE_DATA_BUFFER_SIZE);

    DWORD bytesReturned;
    // fill the reparseStructData
    BOOL isOk = DeviceIoControl(fileHandle, FSCTL_GET_REPARSE_POINT, NULL, 0, reparseStructData,
                                MAXIMUM_REPARSE_DATA_BUFFER_SIZE, &bytesReturned, NULL);
    if (isOk == FALSE) {
        DWORD deviceIOControlError = GetLastError();
        if (deviceIOControlError == ERROR_NOT_A_REPARSE_POINT)
            qDebug() << QString::fromLatin1("Could not reparse information (windows symlink) for %1").arg(path);
        else {
            qDebug() << QString::fromLatin1("Get DeviceIoControl for %1 failed with error: %2").arg(
                path).arg(deviceIOControlError);
        }
        CloseHandle(fileHandle);
        return path;
    }
    CloseHandle(fileHandle);

    QString realPath = path;
    if (IsReparseTagMicrosoft(reparseStructData->ReparseTag)) {
        size_t realPathLength = 0;
        WCHAR *realPathWCHAR = 0;
        if (reparseStructData->ReparseTag == IO_REPARSE_TAG_SYMLINK){
            realPathLength = reparseStructData->SymbolicLinkReparseBuffer.PrintNameLength / sizeof(WCHAR);
            realPathWCHAR = new WCHAR[realPathLength + 1];
            wcsncpy_s(realPathWCHAR, realPathLength + 1, &reparseStructData->SymbolicLinkReparseBuffer.PathBuffer[
                reparseStructData->SymbolicLinkReparseBuffer.PrintNameOffset / sizeof(WCHAR)], realPathLength);
        } else if (reparseStructData->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) {
            realPathLength = reparseStructData->MountPointReparseBuffer.PrintNameLength / sizeof(WCHAR);
            realPathWCHAR = new WCHAR[realPathLength + 1];
            wcsncpy_s(realPathWCHAR, realPathLength + 1, &reparseStructData->MountPointReparseBuffer.PathBuffer[
                reparseStructData->MountPointReparseBuffer.PrintNameOffset / sizeof(WCHAR)], realPathLength);
        } else {
            qDebug() << QString::fromLatin1("Path %1 is not a symlink and not a mount point.").arg(path);
        }
        if (realPathLength != 0) {
            realPathWCHAR[realPathLength] = 0;
            realPath = QString::fromStdWString(realPathWCHAR);
            delete [] realPathWCHAR;
        }

    } else {
        qDebug() << QString::fromLatin1("Path %1 is not reparse point.").arg(path);
    }
    free(reparseStructData);
    return realPath;
}

bool pathIsOnLocalDevice(const QString &path)
{
    if (!QFileInfo(path).exists())
        return false;

    if (path.startsWith(QLatin1String("\\\\")))
        return false;

    QDir dir(path);
    do {
        if (QFileInfo(dir, QString()).isSymLink()) {
            QString currentPath = QFileInfo(dir, QString()).absoluteFilePath();
            return pathIsOnLocalDevice(junctionTargetPath(currentPath));
        }
    } while (dir.cdUp());

    const UINT DRIVE_REMOTE_TYPE = 4;
    if (path.contains(QLatin1Char(':'))) {
        const QLatin1Char nullTermination('\0');
        // for example "c:\"
        const QString driveSearchString = path.left(3) + nullTermination;
        WCHAR wCharDriveSearchArray[4];
        driveSearchString.toWCharArray(wCharDriveSearchArray);
        UINT type = GetDriveType(wCharDriveSearchArray);
        if (type == DRIVE_REMOTE_TYPE)
            return false;
    }

    return true;
}

} // namespace KDUpdater
