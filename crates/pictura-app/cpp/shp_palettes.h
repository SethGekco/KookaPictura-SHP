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
//   4. the same rule over the .pal entries of the MIX archives (see
//      paletteArchives in mix_support.h);
//   5. the default palette;
//   6. unittem.pal inside those archives;
//   7. none: the built-in grey fallback (an empty string).
// A wrong guess costs nothing: indices are kept, and the Palette panel
// rebinds in one undoable step. The choices live in
// $XDG_STATE_HOME/kooka-pictura/shp.ini, beside the session store.
QString resolveShpPalette(const QString& shpPath);

// A TMP terrain template names no palette either; its theater's iso
// palette is the one the game draws it with:
//   1. the terrain palette remembered for the template's folder (kept apart
//      from the SHP one, since a theater folder holds both);
//   2. a .pal beside it with the same stem;
//   3. iso<suffix>.pal (isotem.pal for a .tem, isosno.pal for a .sno, ...)
//      beside it, then under the palette folders (recursive, any case),
//      then inside the MIX archives (see paletteArchives in mix_support.h);
//   4. the default palette, when its name starts with "iso";
//   5. none: the built-in grey fallback.
QString resolveTmpPalette(const QString& tmpPath);

// The theater suffixes TMP templates (and theater SHPs) use.
const QStringList& theaterSuffixes();
bool isTheaterPath(const QString& path);

// `terrain`: the TMP palette of the folder rather than the SHP one.
void rememberFolderPalette(const QString& shpPath, const QString& palettePath,
                           bool terrain = false);
void forgetFolderPalette(const QString& shpPath, bool terrain = false);

QStringList shpPaletteFolders();
void setShpPaletteFolders(const QStringList& folders);

QString defaultShpPalette();
void setDefaultShpPalette(const QString& palettePath);

// The settings file (tests point XDG_STATE_HOME elsewhere).
QString shpSettingsPath();

} // namespace pictura
