// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: which .pal an SHP opens with.
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

namespace pictura {

// An SHP names no palette, so one is chosen when it opens, in this order:
//   1. the palette remembered for the SHP's folder ("use for this folder");
//   2. a .pal beside the SHP with the same stem (sprite.shp + sprite.pal);
//   3. among the .pal files under the palette folders (recursive), the one
//      with the longest stem that starts the SHP's stem (isotem.pal for
//      isotem_bridge.shp);
//   4. the default palette;
//   5. none: the built-in grey fallback (an empty string).
// A wrong guess costs nothing: indices are kept, and the Palette panel
// rebinds in one undoable step. The choices live in
// $XDG_STATE_HOME/kooka-pictura/shp.ini, beside the session store.
QString resolveShpPalette(const QString& shpPath);

void rememberFolderPalette(const QString& shpPath, const QString& palettePath);
void forgetFolderPalette(const QString& shpPath);

QStringList shpPaletteFolders();
void setShpPaletteFolders(const QStringList& folders);

QString defaultShpPalette();
void setDefaultShpPalette(const QString& palettePath);

// The settings file (tests point XDG_STATE_HOME elsewhere).
QString shpSettingsPath();

} // namespace pictura
