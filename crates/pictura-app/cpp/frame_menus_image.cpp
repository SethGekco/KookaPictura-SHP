#include "frame_includes.h"

#include "api/selection_api.h"
#include "canvas_size_dialog.h"
#include "image_size_dialog.h"

#include "pictura_app/src/cxxqt_object/image_adjust/image_mode.cxxqt.h"
#include "pictura_app/src/cxxqt_object/image_adjust/image_ops.cxxqt.h"
#include "pictura_app/src/cxxqt_object/image_adjust/image_size.cxxqt.h"

#include <QtWidgets/QInputDialog>

namespace pictura {

// Image > Crop, Trim, Duplicate, Image Size, and Canvas Size (Image > Mode is frame_menus_image_mode.cpp).
// Lifted out of registerHandlers() to keep frame_menus.cpp within its size
// budget; the handler bodies are unchanged.
void PicturaMainWindow::wireImageMenu()
{
    wireImageModeMenu();

    registry_->setHandler(command_ids::ImageCrop, [this]() {
        if (tools_ && tools_->hasPendingCrop()) {
            commitCrop();
            return;
        }
        PictureView* view = activeView();
        if (!view) {
            return;
        }
        const selection_api::SelectionBounds bounds = selection_api::bounds(*view);
        if (!bounds.valid) {
            return;
        }
        view->crop(bounds.x, bounds.y, bounds.width, bounds.height);
    });
    registry_->setEnabledProvider(command_ids::ImageCrop,
                                  [this]() { return activeView() && activeView()->has_document(); });

    registry_->setHandler(command_ids::ImageTrim, [this]() {
        PictureView* view = activeView();
        if (view) {
            trim_image(*view);
        }
    });
    registry_->setHandler(command_ids::ImageDuplicate, [this]() {
        PictureView* source = activeView();
        if (!source || !source->has_document()) {
            return;
        }
        const QString suggested = duplicate_name(*source);
        DuplicateImageDialog dialog(activeDocumentName(), suggested, this);
        if (runDialog(dialog, this) != QDialog::Accepted) {
            return;
        }
        auto* target = new PictureView(this);
        if (!duplicate_into(*source, *target, dialog.copyName(), dialog.mergedOnly())) {
            delete target;
            return;
        }
        const int index = addDocument(target, QString());
        if (index >= 0) {
            docs_[index]->displayName = dialog.copyName();
            updateTabTitle(index);
        }
    });
    const auto imageEnabled = [this]() {
        return activeView() && activeView()->has_document();
    };
    for (const char* id : {command_ids::ImageTrim, command_ids::ImageDuplicate}) {
        registry_->setEnabledProvider(id, imageEnabled);
    }

    // Resampling keeps each window on the same relative point of the image, so
    // a shrunken image stays centred where the whole of it was.
    const auto applyImageSize = [this](PictureView* view, const QString& resample, int width,
                                       int height, double ppi, bool perCm) {
        const int index = indexOfView(view);
        const QSizeF before(view->document_width(), view->document_height());
        QList<QPair<QPointer<ImageView>, QPointF>> centres;
        if (index >= 0 && !before.isEmpty()) {
            forEachCanvas(*docs_.at(index), [&centres, before](ImageView* canvas) {
                const QPointF centre = canvas->widgetToImage(QRectF(canvas->rect()).center());
                centres.append({canvas, QPointF(centre.x() / before.width(),
                                                centre.y() / before.height())});
            });
        }
        if (!image_size_apply(*view, resample, width, height, ppi, perCm)) {
            return false;
        }
        refresh();
        const QSizeF after(view->document_width(), view->document_height());
        for (const auto& [canvas, fraction] : centres) {
            if (canvas) {
                const QPointF target(fraction.x() * after.width(), fraction.y() * after.height());
                canvas->centerOnImageRect(QRectF(target - QPointF(0.5, 0.5), QSizeF(1.0, 1.0)));
            }
        }
        return true;
    };

    const QString imageSize =
        commandIdForPath({QStringLiteral("Image"), QStringLiteral("Image Size…")});
    registry_->setImplemented(imageSize, true);
    registry_->setEnabledProvider(imageSize, imageEnabled);
    registry_->setHandler(imageSize, [this, imageSize, applyImageSize]() {
        PictureView* view = activeView();
        if (!view || !view->has_document()) {
            return;
        }
        ImageSizeDialog dialog(view, this);
        if (runDialog(dialog, this) == QDialog::Accepted
            && applyImageSize(view, dialog.resampleKind(), dialog.resultWidth(),
                              dialog.resultHeight(), dialog.resultPpi(), dialog.resultPerCm())) {
            CommandParams command;
            command.values.insert(QStringLiteral("width"), dialog.resultWidth());
            command.values.insert(QStringLiteral("height"), dialog.resultHeight());
            command.values.insert(QStringLiteral("resolution"), dialog.resultPpi());
            command.values.insert(QStringLiteral("perCm"), dialog.resultPerCm());
            command.values.insert(QStringLiteral("resample"), dialog.resampleKind());
            registry_->recordCommand(imageSize, command);
        }
    });
    registry_->setExecutor(imageSize, [this, applyImageSize](const CommandParams& params) {
        PictureView* view = activeView();
        if (!view || !view->has_document()) {
            return false;
        }
        const bool constrain =
            params.values.value(QStringLiteral("constrainProportions")).toBool(true);
        int width = params.values.value(QStringLiteral("width")).toInt();
        int height = params.values.value(QStringLiteral("height")).toInt();
        if (constrain && width > 0 && height <= 0) {
            height = qRound(double(view->document_height()) * width / view->document_width());
        } else if (constrain && height > 0 && width <= 0) {
            width = qRound(double(view->document_width()) * height / view->document_height());
        }
        double ppi = params.values.value(QStringLiteral("resolution")).toDouble(-1.0);
        bool perCm = params.values.value(QStringLiteral("perCm")).toBool();
        if (ppi <= 0.0) {
            const QString label = image_resolution(*view);
            ppi = label.section(QLatin1Char(' '), 0, 0).toDouble();
            perCm = label.contains(QStringLiteral("pixels/cm"));
            if (perCm) {
                ppi *= 2.54;
            }
        }
        return applyImageSize(view, params.values.value(QStringLiteral("resample")).toString(),
                              width, height, ppi, perCm);
    });

    const QString canvasSize =
        commandIdForPath({QStringLiteral("Image"), QStringLiteral("Canvas Size…")});
    registry_->setImplemented(canvasSize, true);
    registry_->setEnabledProvider(canvasSize, imageEnabled);
    registry_->setHandler(canvasSize, [this, canvasSize]() {
        PictureView* view = activeView();
        if (!view || !view->has_document()) {
            return;
        }
        CanvasSizeDialog dialog(view, tools_->foreground(), tools_->background(), this);
        if (runDialog(dialog, this) == QDialog::Accepted) {
            const uint fill = dialog.extensionColor().rgb() & 0xffffffu;
            if (!canvas_size_apply(*view, dialog.anchorName(), dialog.resultWidth(),
                                   dialog.resultHeight(), fill)) {
                return;
            }
            CommandParams command;
            command.values.insert(QStringLiteral("width"), dialog.resultWidth());
            command.values.insert(QStringLiteral("height"), dialog.resultHeight());
            command.values.insert(QStringLiteral("anchor"), dialog.anchorName());
            command.values.insert(QStringLiteral("color"), static_cast<double>(fill));
            registry_->recordCommand(canvasSize, command);
        }
    });
    registry_->setExecutor(canvasSize, [this](const CommandParams& params) {
        PictureView* view = activeView();
        if (!view || !view->has_document()) {
            return false;
        }
        const bool applied =
            canvas_size_apply(*view,
                              params.values.value(QStringLiteral("anchor"))
                                  .toString(QStringLiteral("center")),
                              params.values.value(QStringLiteral("width")).toInt(),
                              params.values.value(QStringLiteral("height")).toInt(),
                              uint32_t(params.values.value(QStringLiteral("color")).toDouble()));
        if (applied) {
            refresh();
        }
        return applied;
    });

    registry_->setHandler(command_ids::ImageRotateArbitrary, [this]() {
        PictureView* view = activeView();
        if (!view || !view->has_document()) {
            return;
        }
        bool ok = false;
        const double angle = QInputDialog::getDouble(this, tr("Rotate Image"),
                                                     tr("Angle (degrees):"), 0.0, -359.99, 359.99, 2,
                                                     &ok);
        if (ok && angle != 0.0 && rotate_document_arbitrary(*view, angle)) {
            CommandParams command;
            command.values.insert(QStringLiteral("angle"), angle);
            registry_->recordCommand(command_ids::ImageRotateArbitrary, command);
        }
    });
    registry_->setEnabledProvider(command_ids::ImageRotateArbitrary, imageEnabled);
    registry_->setExecutor(command_ids::ImageRotateArbitrary,
                           [this](const CommandParams& params) {
                               PictureView* view = activeView();
                               const double angle =
                                   params.values.value(QStringLiteral("angle")).toDouble(0.0);
                               if (!view || !view->has_document() || angle == 0.0) {
                                   return false;
                               }
                               const bool applied = rotate_document_arbitrary(*view, angle);
                               if (applied) {
                                   refresh();
                               }
                               return applied;
                           });
}

} // namespace pictura
