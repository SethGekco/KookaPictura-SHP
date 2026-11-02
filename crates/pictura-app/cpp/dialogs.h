#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtCore/QList>

#include <functional>

class QAbstractButton;
class QImage;
class QKeyEvent;
class QComboBox;
class QDialog;
class QWidget;

namespace pictura {

class PictureView;

enum class UnsavedChoice { Save, Discard, Cancel };
enum class TransformDecision;
enum class CropDecision;

// Show `dialog` and block input to `parent`'s top-level window without using a
// modal window hint, so the window compositor's modal-parent dim never fires.
// Returns the dialog's result code. With no parent (tests) this is a plain
// exec(). Every app dialog that used to call `dialog.exec()` should use this.
int runDialog(QDialog& dialog, QWidget* parent);

// While a dialog runs under runDialog, a key press the dialog leaves unused (no
// QShortcut of its own took it) is offered to `handler`, which returns true
// to consume it. The frame installs its canvas zoom keys here, so Ctrl++ /
// Ctrl+- / Ctrl+0 / Ctrl+1 zoom the canvas behind any dialog.
void setDialogNavigationKeys(std::function<bool(QKeyEvent*)> handler);

// While a dialog runs under runDialog and `active` returns true, left-button
// input over a canvas reaches it, so a dialog's eyedropper (an armed canvas
// sampler) can take a colour from the image. The frame installs its tool
// controller's sampler state here.
void setDialogCanvasSampling(std::function<bool()> active);

// Ctrl++ / Ctrl+- in a dialog with its own preview zoom click `zoomIn` /
// `zoomOut`. `+` takes Shift on most layouts, so Ctrl+= and Ctrl+Shift+= zoom
// in too, as they do on the canvas.
void addZoomShortcuts(QWidget* dialog, QAbstractButton* zoomIn, QAbstractButton* zoomOut);

// The whole document as the smallest view-pyramid level whose long side is at
// least `minEdge` (level 0 for a smaller document), so a dialog's small picture
// never builds the full-resolution image. Null without a document.
QImage documentPreviewImage(PictureView* view, int minEdge);

// Ask what to do with a modified document before closing/reverting/replacing it.
// A frame-to-front save prompt is expected when the choice is Save.
UnsavedChoice askUnsaved(QWidget* parent, const QString& documentName);

// Test hook: when interactive is false, askUnsaved does not show a dialog and
// returns the configured choice. Defaults: interactive true, Discard.
void setUnsavedPromptInteractive(bool interactive);
// While set, askUnsaved answers this. A control request sets it for its own
// span, so a person's Close in the same process still prompts.
void setRequestUnsavedChoice(const UnsavedChoice* choice);
void setNonInteractiveUnsavedChoice(UnsavedChoice choice);
// How many times askUnsaved has answered without a dialog, so a test can count
// the prompts an operation raises.
int nonInteractiveUnsavedPromptCount();

// Photoshop's "Apply the transformation?" (Apply / Cancel / Don't Apply), asked
// before a tool switch leaves a pending transform. Non-interactive (the
// default on the offscreen platform: headless runs and tests) it shows nothing
// and returns Don't Apply.
TransformDecision askApplyTransform(QWidget* parent);
void setTransformPromptInteractive(bool interactive);

// "Crop the image?" (Crop / Cancel / Don't Crop), asked before a tool switch
// leaves a pending crop. Non-interactive it shows nothing and returns Don't
// Crop, which is also the default on the offscreen platform.
CropDecision askCropImage(QWidget* parent);
void setCropPromptInteractive(bool interactive);

// Name filters shared by the Open/Save/Place/Open As Smart Object dialogs. One
// filter per format; patterns are space-separated (Qt splits on spaces, so a
// comma would become part of the pattern), uppercase for display.
const QStringList& saveFileFilters();
const QStringList& openFileFilters();
// File > Open As: one filter per decoder, in the order `openAsPath` indexes
// (defined in frame_import.cpp beside the format table).
const QStringList& openAsFileFilters();

// Places shown in the file dialogs' sidebar: root, mounted volumes, the XDG
// folders, KDE/GTK bookmark directories, and the application's recent
// locations. Only directories Qt can browse; KIO places (trash:/, remote:/,
// recentlyused:/) are not representable.
QList<QUrl> fileDialogPlaces();
// Recent files whose parent directory is offered as a recent location.
void setFileDialogRecentPaths(const QStringList& paths);
// True when the app runs inside a sandbox (Flatpak/Snap) or the override is
// set, so the file dialogs hand off to the platform/portal chooser instead of
// the built-in dialog.
bool usesPortalFileDialog();

// Index into saveFileFilters(): PSD unless the document is a single
// pixel/background layer, in which case its remembered source format.
int defaultSaveFilterIndex(PictureView* view);
// True when the document holds content a flat raster format cannot represent
// (more than one layer, a group, an adjustment, or a type layer).
bool documentRequiresLayers(PictureView* view);
// True when Save can write `path` at all: a suffix in saveFileFilters(), or no
// suffix (a PSD). Any other suffix (for example `.foo`) is refused, never
// written as a PSD under that name.
bool isWritableSavePath(const QString& path);
// True when a plain Save to `path` cannot represent the document and must fall
// back to the format-aware Save As dialog: a path Save cannot write, or a
// raster path on a document that needs layers, or an `.shp` path on anything
// but an SHP sheet. `pathIsNative` is true for a PSD/PSB path.
bool savePathNeedsFormatDialog(const QString& path, PictureView* view, bool pathIsNative);
// True when saving to `path` flattens features a flat raster format cannot
// hold -- a raster extension on a document that needs layers.
bool saveAsNeedsFeatureWarning(const QString& path, PictureView* view);
// True for a flat raster extension (png/jpg/jpeg/jpe/tif/tiff/webp/bmp).
bool isRasterImageSuffix(const QString& suffix);
// Lowercase suffix appended when the chosen path has none.
QString defaultSuffixForFilter(int filterIndex);
// Lowercase a known image extension (Qt rewrites it to the filter's uppercase
// pattern when the type changes); leave anything else untouched.
QString lowercasedImageSuffix(const QString& path);
// Save As: a single-format filter owns the extension, so a typed suffix that
// disagrees is replaced with `defaultSuffixForFilter(chosenFilter)`. The All
// Formats entry keeps a typed suffix and appends `defaultSuffixForFilter(
// fallbackFilter)` only when the name has none.
QString pathForSaveFilter(const QString& path, int chosenFilter, int fallbackFilter);

// Qt (non-native) save/open dialogs whose "File type" combo is non-editable and
// supports rolling prefix type-ahead. Return the chosen path (empty = cancel)
// and set `selectedFilter` to the chosen filter.
QString getSaveFileName(QWidget* parent, const QString& caption, const QString& dir,
                        const QStringList& filters, QString* selectedFilter);
QString getOpenFileName(QWidget* parent, const QString& caption, const QString& dir,
                        const QStringList& filters, QString* selectedFilter = nullptr);
// Multi-select variant of getOpenFileName for File > Open: returns every chosen
// path (empty = cancel) and sets `selectedFilter` to the chosen filter.
QStringList getOpenFileNames(QWidget* parent, const QString& caption, const QString& dir,
                             const QStringList& filters, QString* selectedFilter = nullptr);

// Start folder for a dialog opened without an explicit directory. Qt persists
// its own "last visited" folder but reconstructs it as a path from a file://
// URL, so an empty-dir dialog would reset to the working directory on restart;
// this reads the same key with the correct conversion.
QString rememberedFileDialogDirectory();
// The folder an empty-dir dialog should start in: the remembered folder, or the
// user's home directory on first run / when the remembered one is gone.
QString defaultFileDialogDirectory();
// Pure conversion behind the above, exposed for tests: accept a stored
// FileDialog/lastVisited value (a file URL or a plain path) and return the
// folder only when it exists.
QString dialogDirectoryFromStored(const QString& stored);
// Folder holding a document's file, for the "start in the current document's
// folder" tier. Empty document path yields empty so the caller can fall back.
QString documentDirectory(const QString& documentPath);

// Install rolling type-ahead on a non-editable combo (also used for the Export
// As "Format" combo, and by tests).
void installRollingTypeAhead(QComboBox* combo);

} // namespace pictura
