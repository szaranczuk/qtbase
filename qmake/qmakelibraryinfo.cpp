/****************************************************************************
**
** Copyright (C) 2021 The Qt Company Ltd.
** Copyright (C) 2016 Intel Corporation.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtCore module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qmakelibraryinfo.h"

#include <qdir.h>
#include <qfile.h>
#include <qglobalstatic.h>
#include <qsettings.h>
#include <qscopedpointer.h>
#include <qstringlist.h>

#include <qmakeconfig.cpp>

QT_BEGIN_NAMESPACE

// qmake_libraryInfoFile and qmake_abslocation are defined in option.cpp
QString qmake_libraryInfoFile();
QString qmake_abslocation();

struct QMakeLibrarySettings
{
    QMakeLibrarySettings() { load(); }

    void load();

    QScopedPointer<QSettings> settings;
    bool haveDevicePaths;
    bool haveEffectiveSourcePaths;
    bool haveEffectivePaths;
    bool havePaths;
    bool reloadOnQAppAvailable;
};
Q_GLOBAL_STATIC(QMakeLibrarySettings, qmake_library_settings)

QSettings *QMakeLibraryInfo::findConfiguration()
{
    QString qtconfig = qmake_libraryInfoFile();
    if (!qtconfig.isEmpty())
        return new QSettings(qtconfig, QSettings::IniFormat);
    return nullptr; // no luck
}

QSettings *QMakeLibraryInfo::configuration()
{
    QMakeLibrarySettings *ls = qmake_library_settings();
    return ls ? ls->settings.data() : nullptr;
}

void QMakeLibraryInfo::reload()
{
    if (qmake_library_settings.exists())
        qmake_library_settings->load();
}

bool QMakeLibraryInfo::haveGroup(PathGroup group)
{
    QMakeLibrarySettings *ls = qmake_library_settings();
    return ls
            && (group == EffectiveSourcePaths     ? ls->haveEffectiveSourcePaths
                        : group == EffectivePaths ? ls->haveEffectivePaths
                        : group == DevicePaths    ? ls->haveDevicePaths
                                                  : ls->havePaths);
}

void QMakeLibrarySettings::load()
{
    settings.reset(QMakeLibraryInfo::findConfiguration());
    if (settings) {
        QStringList children = settings->childGroups();
        haveDevicePaths = children.contains(QLatin1String("DevicePaths"));
        haveEffectiveSourcePaths = children.contains(QLatin1String("EffectiveSourcePaths"));
        haveEffectivePaths =
                haveEffectiveSourcePaths || children.contains(QLatin1String("EffectivePaths"));
        // Backwards compat: an existing but empty file is claimed to contain the Paths section.
        havePaths = (!haveDevicePaths && !haveEffectivePaths
                     && !children.contains(QLatin1String(platformsSection)))
                || children.contains(QLatin1String("Paths"));
    } else {
        haveDevicePaths = false;
        haveEffectiveSourcePaths = false;
        haveEffectivePaths = false;
        havePaths = false;
    }
}

void QMakeLibraryInfo::sysrootify(QString &path)
{
    // Acceptable values for SysrootifyPrefixPath are "true" and "false"
    if (!QVariant::fromValue(rawLocation(SysrootifyPrefixPath, FinalPaths)).toBool())
        return;

    const QString sysroot = rawLocation(SysrootPath, FinalPaths);
    if (sysroot.isEmpty())
        return;

    if (path.length() > 2 && path.at(1) == QLatin1Char(':')
        && (path.at(2) == QLatin1Char('/') || path.at(2) == QLatin1Char('\\'))) {
        path.replace(0, 2, sysroot); // Strip out the drive on Windows targets
    } else {
        path.prepend(sysroot);
    }
}

static QString getPrefixFromHostBinDir(const char *hostBinDirToPrefixPath)
{
    const QString canonicalQMakePath = QFileInfo(qmake_abslocation()).canonicalPath();
    return QDir::cleanPath(canonicalQMakePath + QLatin1Char('/')
                           + QLatin1String(hostBinDirToPrefixPath));
}

static QString getExtPrefixFromHostBinDir()
{
    return getPrefixFromHostBinDir(QT_CONFIGURE_HOSTBINDIR_TO_EXTPREFIX_PATH);
}

static QString getHostPrefixFromHostBinDir()
{
    return getPrefixFromHostBinDir(QT_CONFIGURE_HOSTBINDIR_TO_HOSTPREFIX_PATH);
}

static QString getPrefix(QMakeLibraryInfo::PathGroup group)
{
#if QT_CONFIGURE_CROSSBUILD
    if (group == QMakeLibraryInfo::DevicePaths)
        return QString::fromLocal8Bit(QT_CONFIGURE_PREFIX_PATH);
#else
    Q_UNUSED(group);
#endif
    return getExtPrefixFromHostBinDir();
}

QString QMakeLibraryInfo::path(int loc)
{
    QString ret = rawLocation(loc, QMakeLibraryInfo::FinalPaths);

    // Automatically prepend the sysroot to target paths
    if (loc < QMakeLibraryInfo::SysrootPath || loc > QMakeLibraryInfo::LastHostPath)
        sysrootify(ret);

    return ret;
}

QString QMakeLibraryInfo::rawLocation(int loc, QMakeLibraryInfo::PathGroup group)
{
    QString ret;
    bool fromConf = false;
    // Logic for choosing the right data source: if EffectivePaths are requested
    // and qt.conf with that section is present, use it, otherwise fall back to
    // FinalPaths. For FinalPaths, use qt.conf if present and contains not only
    // [EffectivePaths], otherwise fall back to builtins.
    // EffectiveSourcePaths falls back to EffectivePaths.
    // DevicePaths falls back to FinalPaths.
    PathGroup orig_group = group;
    if (QMakeLibraryInfo::haveGroup(group)
        || (group == EffectiveSourcePaths
            && (group = EffectivePaths, QMakeLibraryInfo::haveGroup(group)))
        || ((group == EffectivePaths || group == DevicePaths)
            && (group = FinalPaths, QMakeLibraryInfo::haveGroup(group)))
        || (group = orig_group, false)) {
        fromConf = true;

        QString key;
        QString defaultValue;
        if (unsigned(loc) < sizeof(qtConfEntries) / sizeof(qtConfEntries[0])) {
            key = QLatin1String(qtConfEntries[loc].key);
            defaultValue = QLatin1String(qtConfEntries[loc].value);
        }
#ifndef Q_OS_WIN // On Windows we use the registry
        else if (loc == QLibraryInfo::SettingsPath) {
            key = QLatin1String("Settings");
            defaultValue = QLatin1String(".");
        }
#endif
        if (!key.isNull()) {
            QSettings *config = QMakeLibraryInfo::configuration();
            config->beginGroup(QLatin1String(group == DevicePaths ? "DevicePaths"
                                                     : group == EffectiveSourcePaths
                                                     ? "EffectiveSourcePaths"
                                                     : group == EffectivePaths ? "EffectivePaths"
                                                                               : "Paths"));

            ret = config->value(key, defaultValue).toString();

            if (ret.isEmpty()) {
                if (loc == HostPrefixPath)
                    ret = config->value(QLatin1String(qtConfEntries[QLibraryInfo::PrefixPath].key),
                                        QLatin1String(
                                                qtConfEntries[QLibraryInfo::PrefixPath].value))
                                  .toString();
                else if (loc == TargetSpecPath || loc == HostSpecPath
                         || loc == SysrootifyPrefixPath)
                    fromConf = false;
                // The last case here is SysrootPath, which can be legitimately empty.
                // All other keys have non-empty fallbacks to start with.
            }

            // TODO: Might be replaced by common for qmake and qtcore function
            int startIndex = 0;
            forever {
                startIndex = ret.indexOf(QLatin1Char('$'), startIndex);
                if (startIndex < 0)
                    break;
                if (ret.length() < startIndex + 3)
                    break;
                if (ret.at(startIndex + 1) != QLatin1Char('(')) {
                    startIndex++;
                    continue;
                }
                int endIndex = ret.indexOf(QLatin1Char(')'), startIndex + 2);
                if (endIndex < 0)
                    break;
                auto envVarName =
                        QStringView { ret }.mid(startIndex + 2, endIndex - startIndex - 2);
                QString value =
                        QString::fromLocal8Bit(qgetenv(envVarName.toLocal8Bit().constData()));
                ret.replace(startIndex, endIndex - startIndex + 1, value);
                startIndex += value.length();
            }
            config->endGroup();

            ret = QDir::fromNativeSeparators(ret);
        }
    }

    if (!fromConf) {
        // "volatile" here is a hack to prevent compilers from doing a
        // compile-time strlen() on "path". The issue is that Qt installers
        // will binary-patch the Qt installation paths -- in such scenarios, Qt
        // will be built with a dummy path, thus the compile-time result of
        // strlen is meaningless.
        const char *volatile path = nullptr;
        if (loc == QLibraryInfo::PrefixPath) {
            ret = getPrefix(group);
        } else if (unsigned(loc)
                   <= sizeof(qt_configure_str_offsets) / sizeof(qt_configure_str_offsets[0])) {
            path = qt_configure_strs + qt_configure_str_offsets[loc - 1];
#ifndef Q_OS_WIN // On Windows we use the registry
        } else if (loc == QLibraryInfo::SettingsPath) {
            path = QT_CONFIGURE_SETTINGS_PATH;
#endif
        } else if (loc == HostPrefixPath) {
            static const QByteArray hostPrefixPath = getHostPrefixFromHostBinDir().toLatin1();
            path = hostPrefixPath.constData();
        }

        if (path)
            ret = QString::fromLocal8Bit(path);
    }

    // These values aren't actually paths and thus need to be returned verbatim.
    if (loc == TargetSpecPath || loc == HostSpecPath || loc == SysrootifyPrefixPath)
        return ret;

    if (!ret.isEmpty() && QDir::isRelativePath(ret)) {
        QString baseDir;
        if (loc == HostPrefixPath || loc == QLibraryInfo::PrefixPath || loc == SysrootPath) {
            // We make the prefix/sysroot path absolute to the executable's directory.
            // loc == PrefixPath while a sysroot is set would make no sense here.
            // loc == SysrootPath only makes sense if qmake lives inside the sysroot itself.
            baseDir = QFileInfo(qmake_libraryInfoFile()).absolutePath();
        } else if (loc > SysrootPath && loc <= LastHostPath) {
            // We make any other host path absolute to the host prefix directory.
            baseDir = rawLocation(HostPrefixPath, group);
        } else {
            // we make any other path absolute to the prefix directory
            baseDir = rawLocation(QLibraryInfo::PrefixPath, group);
            if (group == EffectivePaths)
                sysrootify(baseDir);
        }
        ret = QDir::cleanPath(baseDir + QLatin1Char('/') + ret);
    }
    return ret;
}

QT_END_NAMESPACE
