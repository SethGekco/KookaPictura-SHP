// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the Unit panel of an SHP document (the shadow half
// of a unit sprite, the house-color preview, the radar color).
#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QPair>
#include <QtCore/QPointer>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtWidgets/QWidget>

#include <array>

#include "image_view.h"

class QAction;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;

namespace pictura {

class PictureView;
class ShpFramesPanel;

// TS/RA2 unit sprites hold their shadows in the second half of the frames.
// "Shadow frames" says whether this sprite does (found from the pixels the
// first time a document is shown, then up to the user, per document); the
// Frames panel marks those frames and Save warns about shadow frames that
// hold colors other than index 0 and 1. Generate casts shadows from the
// art (index 1, with an offset and a lean); Clear empties shadow frames.
// The underlay shows each frame's shadow under it, view only.
//
// House color: the remap indices (16-31 of a unit palette) previewed in a
// multiplayer color or one of the user's, view only and approximate (the
// palette's own shading of the ramp, not the game's formula).
//
// Radar color: each frame's (the minimap color) as the file has it and the
// average of its pixels now. Save keeps the file's unless "Recompute radar
// colors on save" is on for the document.
class ShpUnitPanel : public QWidget {
    Q_OBJECT

public:
    explicit ShpUnitPanel(ShpFramesPanel* frames, QWidget* parent = nullptr);

    void setView(PictureView* view);
    void refresh();

    // The first shadow frame of `view` while its second half counts as
    // shadow frames (an even frame count), else 0.
    static int shadowHalfOf(PictureView* view);
    // Whether `view`'s second half counts as shadow frames; decided from
    // the pixels (shp_shadow_detect) the first time it is asked.
    static bool shadowsOf(PictureView* view);
    static void setShadowsOf(PictureView* view, bool on);
    int shadowHalf() const;

    // The art frames "Generate" works on (`all`: every art frame; else the
    // selected frames, a shadow frame standing for its art frame) and the
    // shadow frames "Clear" works on.
    QList<int> artFrames(bool all) const;
    QList<int> shadowFrames(bool all) const;

    QCheckBox* shadowToggle() const { return shadows_; }
    QCheckBox* underlay() const { return underlay_; }
    QSpinBox* offsetX() const { return dx_; }
    QSpinBox* offsetY() const { return dy_; }
    QDoubleSpinBox* skew() const { return skew_; }
    QLabel* shadowReadout() const { return readout_; }
    // generate, generateAll, clear, clearAll.
    QAction* action(const QString& name) const { return actions_.value(name); }

    // The preset house colors: the stock Yuri's Revenge multiplayer colors.
    static QList<QPair<QString, QColor>> housePresets();
    QCheckBox* houseToggle() const { return house_; }
    QComboBox* houseColors() const { return houseColors_; }
    QColor houseColor() const { return houseColor_; }
    // Preview in `color` (a preset when it is one, else "Custom").
    void setHouseColor(const QColor& color);

    // Whether saving `view` recomputes its radar colors (per document, off
    // until turned on).
    static bool recomputeRadarOf(PictureView* view);
    QCheckBox* recomputeRadar() const { return recomputeRadar_; }
    QLabel* radarFile() const { return radarFile_; }
    QLabel* radarAverage() const { return radarAverage_; }

signals:
    void generateRequested(const QList<int>& art, int dx, int dy, double skew);
    void clearRequested(const QList<int>& frames);
    void message(const QString& text);

private:
    void decorate(ImageView::ShpGrid& grid);
    void sync();
    void showRadar(int frame);
    QImage offSheetShadow(int frame);

    ShpFramesPanel* frames_ = nullptr;
    QPointer<PictureView> view_;
    QCheckBox* shadows_ = nullptr;
    QCheckBox* underlay_ = nullptr;
    QSpinBox* dx_ = nullptr;
    QSpinBox* dy_ = nullptr;
    QDoubleSpinBox* skew_ = nullptr;
    QLabel* readout_ = nullptr;
    QHash<QString, QAction*> actions_;
    QCheckBox* house_ = nullptr;
    QComboBox* houseColors_ = nullptr;
    QLabel* houseSwatch_ = nullptr;
    QColor houseColor_;
    QCheckBox* recomputeRadar_ = nullptr;
    QLabel* radarFile_ = nullptr;
    QLabel* radarAverage_ = nullptr;
    // What the radar readout shows: frame, canvas revision, generation.
    std::array<quint64, 3> radarKey_{~quint64(0), 0, 0};
    // The active frame's shadow when it is off a paged sheet: frame, stamp.
    QPair<int, quint64> shadowKey_{-1, 0};
    QImage shadowImage_;
};

} // namespace pictura
