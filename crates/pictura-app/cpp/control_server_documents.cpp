#include "control_server.h"
#include "control_server_support.h"

#include "api/history_api.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>

#include "dialogs.h"
#include "frame.h"

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/image_adjust/new_document.cxxqt.h"

namespace pictura {

using control_detail::documentInfo;
using control_detail::pathProblem;

namespace {

// Mirrors the dialog branches of PicturaMainWindow::saveActive().
bool activeSaveNeedsDialog(PicturaMainWindow* frame)
{
    const QString path = frame->activeFilePath();
    return !path.isEmpty()
        && (PicturaMainWindow::isPdfPath(path)
            || savePathNeedsFormatDialog(path, frame->activeView(),
                                         PicturaMainWindow::isNativeDocumentPath(path)));
}

} // namespace

namespace control_detail {

QString pathProblem(const QString& path, bool writes)
{
    if (!QDir::isAbsolutePath(path)) {
        return QStringLiteral("path must be absolute");
    }
    for (const QChar c : path) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f) {
            return QStringLiteral("path must not contain control characters");
        }
    }
    if (QDir::fromNativeSeparators(path).split(QLatin1Char('/')).contains(QStringLiteral(".."))) {
        return QStringLiteral("path must not contain '..'");
    }
    if (writes && QFileInfo(path).isSymLink()) {
        return QStringLiteral("path must not be a symbolic link");
    }
    return {};
}

} // namespace control_detail

QJsonObject ControlServer::methodDocument(const QJsonObject& params)
{
    const QString op = params.value(QStringLiteral("op")).toString();
    if (op.isEmpty()) {
        return error(QStringLiteral("invalid_param"), QStringLiteral("op is required"));
    }

    if (op == QStringLiteral("list")) {
        QJsonArray documents;
        for (int i = 0; i < frame_->documentCount(); ++i) {
            documents.append(documentInfo(frame_, i));
        }
        QJsonObject result;
        result.insert(QStringLiteral("documents"), documents);
        result.insert(QStringLiteral("active"), frame_->activeDocumentIndex());
        result.insert(QStringLiteral("count"), frame_->documentCount());
        return ok(result);
    }

    bool applied = false;
    if (op == QStringLiteral("open")) {
        const QString path = params.value(QStringLiteral("path")).toString();
        if (path.isEmpty()) {
            return error(QStringLiteral("invalid_param"), QStringLiteral("path is required"));
        }
        if (const QString problem = pathProblem(path, false); !problem.isEmpty()) {
            return error(QStringLiteral("invalid_param"), problem);
        }
        const PicturaMainWindow::OpenOutcome outcome =
            frame_->openDocumentAtPathOutcome(path, false);
        applied = outcome == PicturaMainWindow::OpenOutcome::Opened;
        if (!applied) {
            return error(outcome == PicturaMainWindow::OpenOutcome::Cancelled
                             ? QStringLiteral("cancelled")
                             : QStringLiteral("io_error"),
                         QStringLiteral("could not open %1").arg(path));
        }
    } else if (op == QStringLiteral("new")) {
        const int width = params.value(QStringLiteral("width")).toInt(0);
        const int height = params.value(QStringLiteral("height")).toInt(0);
        if (width <= 0 || height <= 0) {
            return error(QStringLiteral("invalid_param"),
                         QStringLiteral("width and height must be positive"));
        }
        const QString name =
            params.value(QStringLiteral("name")).toString(QStringLiteral("Untitled"));
        if (name.size() > kMaxNameChars) {
            return error(QStringLiteral("invalid_param"), QStringLiteral("name is too long"));
        }
        const QString mode =
            params.value(QStringLiteral("mode")).toString(QStringLiteral("rgb"));
        const int depth = params.value(QStringLiteral("depth")).toInt(8);
        // Allocation failure aborts the process, so judge the size up front.
        if (!document_budget_allows(width, height, depth)) {
            return error(QStringLiteral("invalid_param"),
                         QStringLiteral("document exceeds the image budget"));
        }
        const QString background =
            params.value(QStringLiteral("background")).toString(QStringLiteral("white"));
        applied = frame_->newDocument(name, width, height, mode, depth, background);
        if (!applied) {
            return error(QStringLiteral("internal"), QStringLiteral("could not create document"));
        }
    } else if (op == QStringLiteral("save") || op == QStringLiteral("save_as")) {
        if (!frame_->activeView()) {
            return error(QStringLiteral("no_document"), QStringLiteral("no active document"));
        }
        const QString path = params.value(QStringLiteral("path")).toString();
        if (const QString problem = path.isEmpty() ? QString() : pathProblem(path, true);
            !problem.isEmpty()) {
            return error(QStringLiteral("invalid_param"), problem);
        }
        if (!path.isEmpty() && !isWritableSavePath(path)) {
            return error(QStringLiteral("invalid_param"),
                         QStringLiteral("cannot save .%1 files; use a suffix from the Save As "
                                        "formats").arg(QFileInfo(path).suffix()));
        }
        if (op == QStringLiteral("save_as")) {
            if (path.isEmpty()) {
                return error(QStringLiteral("invalid_param"), QStringLiteral("path is required"));
            }
            applied = frame_->saveActiveAs(path);
        } else if (path.isEmpty()) {
            // `saveActive()` opens Save-As for an untitled document; refuse
            // instead of reaching a modal file dialog.
            if (frame_->activeFilePath().isEmpty()) {
                return error(QStringLiteral("invalid_param"),
                             QStringLiteral("save requires a path for an untitled document"));
            }
            // `open` keeps the path as given, so the active path can be a link.
            if (QFileInfo(frame_->activeFilePath()).isSymLink()) {
                return error(QStringLiteral("invalid_param"),
                             QStringLiteral("path must not be a symbolic link"));
            }
            if (activeSaveNeedsDialog(frame_)) {
                return error(QStringLiteral("refused"),
                             QStringLiteral("this save needs a dialog; use save_as"));
            }
            applied = frame_->saveActive();
        } else {
            applied = frame_->saveActiveAs(path);
        }
        if (!applied) {
            return error(QStringLiteral("io_error"), QStringLiteral("save failed"));
        }
    } else if (op == QStringLiteral("revert")) {
        if (!frame_->activeView()) {
            return error(QStringLiteral("no_document"), QStringLiteral("no active document"));
        }
        applied = frame_->revertActive();
        if (!applied) {
            return error(QStringLiteral("io_error"), QStringLiteral("revert failed"));
        }
    } else if (op == QStringLiteral("close")) {
        const int index = params.contains(QStringLiteral("index"))
                              ? params.value(QStringLiteral("index")).toInt(-1)
                              : frame_->activeDocumentIndex();
        if (index < 0 || index >= frame_->documentCount()) {
            return error(QStringLiteral("invalid_param"), QStringLiteral("invalid document index"));
        }
        applied = frame_->closeDocument(index, false);
        if (!applied) {
            return error(QStringLiteral("internal"), QStringLiteral("close failed"));
        }
    } else if (op == QStringLiteral("activate")) {
        const int index = params.value(QStringLiteral("index")).toInt(-1);
        if (index < 0 || index >= frame_->documentCount()) {
            return error(QStringLiteral("invalid_param"), QStringLiteral("invalid document index"));
        }
        frame_->setActiveDocumentIndex(index);
        applied = frame_->activeDocumentIndex() == index;
    } else {
        return error(QStringLiteral("invalid_param"),
                     QStringLiteral("unknown op: %1").arg(op));
    }

    return documentResult(applied);
}

QJsonObject ControlServer::documentResult(bool applied) const
{
    QJsonObject result;
    result.insert(QStringLiteral("applied"), applied);
    result.insert(QStringLiteral("active"), frame_->activeDocumentIndex());
    result.insert(QStringLiteral("count"), frame_->documentCount());
    result.insert(QStringLiteral("path"), frame_->activeFilePath());
    result.insert(QStringLiteral("name"), frame_->activeDocumentName());
    return ok(result);
}

QJsonObject ControlServer::methodEdit(const QJsonObject& params)
{
    PictureView* view = frame_->activeView();
    if (!view || !view->has_document()) {
        return error(QStringLiteral("no_document"), QStringLiteral("no active document"));
    }
    const QString op = params.value(QStringLiteral("op")).toString();
    if (params.contains(QStringLiteral("expected_index"))
        && params.value(QStringLiteral("expected_index")).toInt(-1) != history_api::index(*view)) {
        return error(QStringLiteral("stale_history"),
                     QStringLiteral("history has moved since that index"));
    }
    bool applied = false;
    if (op == QStringLiteral("undo")) {
        applied = history_api::undo(*view);
    } else if (op == QStringLiteral("redo")) {
        applied = history_api::redo(*view);
    } else {
        return error(QStringLiteral("invalid_param"),
                     QStringLiteral("op must be undo or redo"));
    }
    QJsonObject result;
    result.insert(QStringLiteral("success"), applied);
    result.insert(QStringLiteral("can_undo"), history_api::canUndo(*view));
    result.insert(QStringLiteral("can_redo"), history_api::canRedo(*view));
    result.insert(QStringLiteral("depth"), history_api::depth(*view));
    result.insert(QStringLiteral("index"), history_api::index(*view));
    return ok(result);
}

QJsonObject ControlServer::methodSetUnsavedPolicy(const QJsonObject& params)
{
    bool interactive = params.contains(QStringLiteral("interactive"))
                           ? params.value(QStringLiteral("interactive")).toBool(unsavedInteractive_)
                           : unsavedInteractive_;
    QString choice = params.contains(QStringLiteral("choice"))
                         ? params.value(QStringLiteral("choice")).toString(unsavedChoice_)
                         : unsavedChoice_;
    if (choice != QStringLiteral("cancel") && choice != QStringLiteral("discard")) {
        return error(QStringLiteral("invalid_param"),
                     QStringLiteral("choice must be cancel or discard"));
    }
    setUnsavedPromptInteractive(interactive);
    if (params.contains(QStringLiteral("choice"))) {
        setNonInteractiveUnsavedChoice(choice == QStringLiteral("cancel")
                                           ? UnsavedChoice::Cancel
                                           : UnsavedChoice::Discard);
    }
    unsavedInteractive_ = interactive;
    unsavedChoice_ = choice;
    QJsonObject result;
    result.insert(QStringLiteral("interactive"), interactive);
    result.insert(QStringLiteral("choice"), choice);
    return ok(result);
}

} // namespace pictura
