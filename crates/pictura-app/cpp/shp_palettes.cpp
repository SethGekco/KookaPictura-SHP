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

QString folderKey(const QString& shpPath)
{
    const QFileInfo info(shpPath);
    const QString dir = info.absoluteDir().canonicalPath().isEmpty()
                            ? info.absolutePath()
                            : info.absoluteDir().canonicalPath();
    return QStringLiteral("FolderPalette/")
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

void rememberFolderPalette(const QString& shpPath, const QString& palettePath)
{
    settings().setValue(folderKey(shpPath), palettePath);
}

void forgetFolderPalette(const QString& shpPath)
{
    settings().remove(folderKey(shpPath));
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
