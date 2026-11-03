// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the save gate of a TMP terrain template, and new
// templates made from images.

#include "shp_support.h"

#include "frame.h"
#include "panels/tmp_tiles_panel.h"
#include "shp_palettes.h"
#include "tmp_authoring.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtGui/QAction>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/shp.cxxqt.h"

namespace pictura {

namespace {

QString slotList(const ::rust::Vec<int32_t>& tiles)
{
    QStringList names;
    for (size_t i = 0; i < tiles.size() && i < 8; ++i) {
        names << QString::number(tiles[i]);
    }
    if (tiles.size() > 8) {
        names << QStringLiteral("...");
    }
    return names.join(QStringLiteral(", "));
}

QPoint pointOf(const ::rust::Vec<int32_t>& xy)
{
    return xy.size() == 2 ? QPoint(xy[0], xy[1]) : QPoint(-1, -1);
}

} // namespace

bool ShpSupport::tmpGate(PictureView* view, bool dialogs, const QString& path)
{
    gateMessage_.clear();
    const QString title = tr("Save as TMP");
    const bool asks = dialogs && interactive();
    const auto refuse = [this, asks, title](const QString& text) {
        gateMessage_ = text;
        message(text);
        if (asks) {
            QMessageBox::warning(frame_, title, text);
        }
        return false;
    };
    if (!path.isEmpty() && !isTmpPath(path)) {
        return refuse(tr("A TMP terrain template saves as a TMP (.tem, .sno, .urb, .ubn, .des, "
                         ".lun or .tmp), not as .%1.")
                          .arg(QFileInfo(path).suffix().toLower()));
    }
    // Show Me: the tile (or the pixel) in view and the pixel selected.
    const auto show = [this, view](const QPoint& at, int slot) {
        dock_->show();
        if (slot >= 0) {
            tiles_->focusSlot(slot);
        }
        if (at.x() >= 0) {
            tiles_->focusPoint(at);
            view->select_rect(at.x(), at.y(), 1, 1, QStringLiteral("new"), 0.0);
        }
    };
    // One question: [fix] / [Show Me] / [Cancel]; true when the fix was
    // chosen.
    const auto ask = [this, title](const QString& text, const QString& info, const QString& fix,
                                   const QString& fixName, bool* showMe) {
        QMessageBox box(QMessageBox::Warning, title, text, QMessageBox::NoButton, frame_);
        box.setInformativeText(info);
        QPushButton* go = box.addButton(fix, QMessageBox::AcceptRole);
        go->setObjectName(fixName);
        QPushButton* look = box.addButton(tr("Show Me"), QMessageBox::ActionRole);
        look->setObjectName(QStringLiteral("shpGateShow"));
        QPushButton* cancel = box.addButton(QMessageBox::Cancel);
        cancel->setObjectName(QStringLiteral("shpGateCancel"));
        box.setDefaultButton(cancel);
        box.setEscapeButton(cancel);
        box.setObjectName(QStringLiteral("shpGate"));
        box.exec();
        *showMe = box.clickedButton() == look;
        return box.clickedButton() == go;
    };

    TmpCheck check = tmp_check(*view);
    if (!check.error.isEmpty()) {
        return refuse(QString(check.error));
    }
    if (check.off_palette > 0 || check.off_scale > 0) {
        QStringList parts;
        if (check.off_palette > 0) {
            parts << tr("%n pixel(s) are not palette colours", nullptr, check.off_palette);
        }
        if (check.off_scale > 0) {
            parts << tr("%n pixel(s) on a Z layer are not depths 0-31", nullptr, check.off_scale);
        }
        const QString text =
            tr("%1 (tiles %2).").arg(parts.join(tr(" and ")), slotList(check.off_slots));
        if (!asks) {
            return refuse(text + tr(" Snap them before saving."));
        }
        bool showMe = false;
        const bool snap = ask(
            text,
            tr("Snap maps each colour to the nearest palette index (anything mostly transparent "
               "becomes index 0) and each Z grey to the nearest depth. It is recorded in "
               "History, so what is saved is what you see."),
            tr("Snap and Save"), QStringLiteral("shpGateSnap"), &showMe);
        if (showMe) {
            show(pointOf(check.first_off),
                 check.off_slots.empty() ? -1 : int(check.off_slots[0]));
        }
        if (!snap) {
            gateMessage_ = text;
            return false;
        }
        if (tmp_snap(*view) < 0) {
            return refuse(lastSaveError());
        }
        check = tmp_check(*view);
    }
    if (check.outside > 0) {
        const QString text = tr("%n pixel(s) lie outside every tile's diamond and extra block "
                                "and would be lost.",
                                nullptr, check.outside);
        if (!asks) {
            return refuse(text + tr(" Clip them before saving."));
        }
        bool showMe = false;
        const bool clip = ask(text,
                              tr("Clipping clears them; it is recorded in History. The tile "
                                 "outlines show where each layer's tiles are."),
                              tr("Clip and Save"), QStringLiteral("shpGateClip"), &showMe);
        if (showMe) {
            show(pointOf(check.first_outside), -1);
        }
        if (!clip) {
            gateMessage_ = text;
            return false;
        }
        if (tmp_clip(*view) < 0) {
            return refuse(lastSaveError());
        }
    }
    return true;
}

void ShpSupport::installTmpMenus(QMenu* menu)
{
    newTmpAction_ = new QAction(tr("New TMP from Image…"), this);
    newTmpAction_->setObjectName(QStringLiteral("tmpNewFromImageAction"));
    newTmpAction_->setToolTip(tr("Make a TMP terrain template of the active image"));
    connect(newTmpAction_, &QAction::triggered, this, [this] {
        auto* window = qobject_cast<PicturaMainWindow*>(frame_);
        PictureView* source = window ? window->activeView() : nullptr;
        if (!source) {
            message(tr("Open the image to make a TMP template of first."));
            return;
        }
        tmpFromImageWithDialog(source, QString(), true);
    });
    const QList<QAction*> actions = menu->actions();
    const qsizetype at = actions.indexOf(newShpAction_);
    menu->insertAction(actions.value(at + 1), newTmpAction_);
}

bool ShpSupport::newTmpFromImage(PictureView* source, const QList<int>& params,
                                 const QList<int>& cells, const QString& palette,
                                 const QString& name)
{
    gateMessage_.clear();
    auto* window = qobject_cast<PicturaMainWindow*>(frame_);
    if (!window || !source) {
        gateMessage_ = tr("no image");
        return false;
    }
    auto* view = new PictureView(window);
    const std::vector<int32_t> p(params.begin(), params.end());
    const std::vector<int32_t> c(cells.begin(), cells.end());
    const ::rust::Vec<int32_t> made =
        tmp_from_image(*view, *source, name, ::rust::Slice<const int32_t>(p.data(), p.size()),
                       ::rust::Slice<const int32_t>(c.data(), c.size()), palette,
                       allowanceOf(view));
    if (made.size() != 5) {
        delete view;
        gateMessage_ = shp_last_error();
        message(gateMessage_);
        return false;
    }
    window->addDocument(view, QString());
    QString text = tr("New TMP template: %n tile(s), %1 x %2 px.", nullptr, made[0])
                       .arg(made[3])
                       .arg(made[4]);
    if (made[1] > 0) {
        text += tr(" %n pixel(s) lie outside the tiles; Save offers to clip them.", nullptr,
                   made[1]);
    }
    if (made[2] > 0) {
        text += tr(" %n pixel(s) fell off the template's canvas and were left out.", nullptr,
                   made[2]);
    }
    gateMessage_ = text;
    message(text);
    return true;
}

bool ShpSupport::tmpFromImageWithDialog(PictureView* source, const QString& path, bool dialogs)
{
    gateMessage_.clear();
    savedCopy_ = false;
    auto* window = qobject_cast<PicturaMainWindow*>(frame_);
    if (!window || !source || !dialogs || !asksNow()) {
        gateMessage_ = tr("This image is not a TMP template. Saving it as TMP makes a new "
                          "template with an options dialog (grid, tiles, palette), which needs "
                          "an interactive session.");
        message(gateMessage_);
        return false;
    }
    const QString suffix = QFileInfo(path).suffix().toLower();
    const QString theater = theaterSuffixes().contains(suffix) ? suffix : QString();
    QString sourcePath;
    for (int i = 0; i < window->documentCount(); ++i) {
        if (window->viewAt(i) == source) {
            sourcePath = window->documentPath(i);
        }
    }
    const QFileInfo base(path.isEmpty() ? sourcePath : path);
    const QString stem = base.completeBaseName().isEmpty() ? QStringLiteral("untitled")
                                                           : base.completeBaseName();
    const QDir dir = base.filePath().isEmpty() ? QDir::current() : base.absoluteDir();
    TmpFromImageDialog dialog(
        source->document_width(), source->document_height(),
        [source](int cx, int cy, int bx, int by) {
            QList<bool> out;
            for (const bool on : tmp_image_cells(*source, cx, cy, bx, by)) {
                out << on;
            }
            return out;
        },
        [dir, stem](const QString& theaterSuffix) {
            return resolveTmpPalette(dir.filePath(stem + QLatin1Char('.') + theaterSuffix));
        },
        theater, frame_);
    if (dialog.exec() != QDialog::Accepted) {
        gateMessage_ = tr("New TMP from Image was cancelled.");
        return false;
    }
    if (!newTmpFromImage(source, dialog.params(), dialog.cells(), dialog.palette(),
                         path.isEmpty() ? stem : QFileInfo(path).fileName())) {
        QMessageBox::warning(frame_, tr("New TMP from Image"), gateMessage_);
        return false;
    }
    if (path.isEmpty()) {
        return true;
    }
    // The new template is active now: the normal save, its gate included.
    const QString made = gateMessage_;
    savedCopy_ = window->saveActiveAs(path);
    if (savedCopy_) {
        gateMessage_ = tr("Saved %1 from a new TMP template document; the original image is "
                          "unchanged. %2")
                           .arg(QFileInfo(path).fileName(), made);
        message(gateMessage_);
    }
    return false;
}

} // namespace pictura
