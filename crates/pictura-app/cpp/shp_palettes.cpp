// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: which .pal an SHP opens with (see shp_palettes.h).

#include "shp_palettes.h"

#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QUrl>

namespace pictura {

namespace {

// More .pal files than any extracted game tree holds; bounds a mistaken
// folder such as $HOME.
constexpr int kMaxPalettesScanned = 4000;

QString folderKey(const QString& shpPath, bool terrain = false)
{
    const QFileInfo info(shpPath);
    const QString dir = info.absoluteDir().canonicalPath().isEmpty()
                            ? info.absolutePath()
                            : info.absoluteDir().canonicalPath();
    return (terrain ? QStringLiteral("TerrainFolderPalette/") : QStringLiteral("FolderPalette/"))
        + QString::fromLatin1(QUrl::toPercentEncoding(dir));
}

QString existingFile(const QString& path)
{
    return !path.isEmpty() && QFileInfo(path).isFile() ? path : QString();
}

} // namespace

QString shpSettingsPath()
{
    const QString base = qEnvironmentVariable("XDG_STATE_HOME");
    const QString root =
        base.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/state") : base;
    return root + QStringLiteral("/kooka-pictura/shp.ini");
}

static QSettings settings()
{
    return QSettings(shpSettingsPath(), QSettings::IniFormat);
}

QString resolveShpPalette(const QString& shpPath)
{
    const QFileInfo shp(shpPath);
    if (const QString remembered = existingFile(settings().value(folderKey(shpPath)).toString());
        !remembered.isEmpty()) {
        return remembered;
    }
    const QString stem = shp.completeBaseName();
    for (const QString& suffix : {QStringLiteral(".pal"), QStringLiteral(".PAL")}) {
        if (const QString beside = existingFile(shp.absoluteDir().filePath(stem + suffix));
            !beside.isEmpty()) {
            return beside;
        }
    }
    QString best;
    int bestLength = 0;
    int scanned = 0;
    for (const QString& folder : shpPaletteFolders()) {
        QDirIterator it(folder, {QStringLiteral("*.pal"), QStringLiteral("*.PAL")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext() && scanned < kMaxPalettesScanned) {
            const QFileInfo pal(it.next());
            ++scanned;
            const QString palStem = pal.completeBaseName();
            if (palStem.size() > bestLength
                && stem.startsWith(palStem, Qt::CaseInsensitive)) {
                best = pal.absoluteFilePath();
                bestLength = int(palStem.size());
            }
        }
    }
    if (!best.isEmpty()) {
        return best;
    }
    return existingFile(defaultShpPalette());
}

void rememberFolderPalette(const QString& shpPath, const QString& palettePath, bool terrain)
{
    settings().setValue(folderKey(shpPath, terrain), palettePath);
}

void forgetFolderPalette(const QString& shpPath, bool terrain)
{
    settings().remove(folderKey(shpPath, terrain));
}

const QStringList& theaterSuffixes()
{
    static const QStringList suffixes = {
        QStringLiteral("tem"), QStringLiteral("sno"), QStringLiteral("urb"),
        QStringLiteral("ubn"), QStringLiteral("des"), QStringLiteral("lun"),
    };
    return suffixes;
}

bool isTheaterPath(const QString& path)
{
    return theaterSuffixes().contains(QFileInfo(path).suffix().toLower());
}

QString resolveTmpPalette(const QString& tmpPath)
{
    const QFileInfo tmp(tmpPath);
    if (const QString remembered =
            existingFile(settings().value(folderKey(tmpPath, true)).toString());
        !remembered.isEmpty()) {
        return remembered;
    }
    const QDir dir = tmp.absoluteDir();
    const QString stem = tmp.completeBaseName();
    for (const QString& suffix : {QStringLiteral(".pal"), QStringLiteral(".PAL")}) {
        if (const QString beside = existingFile(dir.filePath(stem + suffix)); !beside.isEmpty()) {
            return beside;
        }
    }
    const QString suffix = tmp.suffix().toLower();
    if (theaterSuffixes().contains(suffix)) {
        const QString wanted = QStringLiteral("iso") + suffix + QStringLiteral(".pal");
        const QStringList beside =
            dir.entryList({QStringLiteral("*.pal"), QStringLiteral("*.PAL")}, QDir::Files);
        for (const QString& name : beside) {
            if (name.compare(wanted, Qt::CaseInsensitive) == 0) {
                return dir.filePath(name);
            }
        }
        int scanned = 0;
        for (const QString& folder : shpPaletteFolders()) {
            QDirIterator it(folder, {QStringLiteral("*.pal"), QStringLiteral("*.PAL")},
                            QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext() && scanned < kMaxPalettesScanned) {
                const QFileInfo pal(it.next());
                ++scanned;
                if (pal.fileName().compare(wanted, Qt::CaseInsensitive) == 0) {
                    return pal.absoluteFilePath();
                }
            }
        }
    }
    const QString fallback = existingFile(defaultShpPalette());
    return QFileInfo(fallback).fileName().startsWith(QStringLiteral("iso"), Qt::CaseInsensitive)
               ? fallback
               : QString();
}

QStringList shpPaletteFolders()
{
    return settings().value(QStringLiteral("PaletteFolders")).toStringList();
}

void setShpPaletteFolders(const QStringList& folders)
{
    settings().setValue(QStringLiteral("PaletteFolders"), folders);
}

QString defaultShpPalette()
{
    return settings().value(QStringLiteral("DefaultPalette")).toString();
}

void setDefaultShpPalette(const QString& palettePath)
{
    settings().setValue(QStringLiteral("DefaultPalette"), palettePath);
}

} // namespace pictura
