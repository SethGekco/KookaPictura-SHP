// The editing surface an agent needs beyond one-shot defaults: parameters for
// filters, adjustments and commands, layer paths, export, and history.

#include "control_server.h"
#include "control_server_support.h"

#include "api/document_api.h"
#include "api/history_api.h"

#include <QtCore/QHash>
#include <QtCore/QJsonArray>

#include <cmath>

#include "commands.h"
#include "filter_commands.h"
#include "frame.h"
#include "shp_support.h"

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/export.cxxqt.h"
#include "pictura_app/src/cxxqt_object/filter_tools.cxxqt.h"
#include "pictura_app/src/cxxqt_object/image_adjust.cxxqt.h"
#include "pictura_app/src/cxxqt_object/image_adjust/new_document.cxxqt.h"
#include "pictura_app/src/cxxqt_object/layer_path.cxxqt.h"

namespace pictura {

using control_detail::pathProblem;

namespace {

::rust::Slice<const std::uint8_t> slice(const QByteArray& bytes)
{
    return {reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size())};
}

QByteArray bytes(const ::rust::Vec<std::uint8_t>& v)
{
    return QByteArray(reinterpret_cast<const char*>(v.data()), qsizetype(v.size()));
}

bool needsResource(const FilterCommandSpec& spec)
{
    if (spec.mapAfterOk || spec.dialog != FilterDialogKind::Shared) {
        return true;
    }
    for (const FilterParamSpec& param : spec.params) {
        if (param.control == FilterControl::Hidden) {
            return true;
        }
    }
    return false;
}

// Slots this control's range covers; the rest are finite-checked only, and the
// engine bounds them. Empty means the value is acceptable.
QString slotProblem(const FilterParamSpec& param, double value)
{
    double low = param.minimum;
    double high = param.maximum;
    switch (param.control) {
    case FilterControl::Choice:
    case FilterControl::Radio:
        low = 0.0;
        high = double(qMax(param.choices.size() - 1, 0));
        break;
    case FilterControl::CheckBox:
        low = 0.0;
        high = 1.0;
        break;
    case FilterControl::Color:
        low = 0.0;
        high = 255.0;
        break;
    case FilterControl::Placement:
    case FilterControl::BlurCenter:
        low = 0.0;
        high = 1.0;
        break;
    case FilterControl::Slider:
    case FilterControl::Angle:
    case FilterControl::Number:
        break;
    default:
        return {};
    }
    if (value < low || value > high) {
        return QStringLiteral("%1 must be in %2..%3").arg(param.label).arg(low).arg(high);
    }
    return {};
}

QString filterValuesProblem(const FilterCommandSpec& spec, const QList<double>& values)
{
    int expected = 0;
    for (const FilterParamSpec& param : spec.params) {
        expected += param.slotCount;
    }
    if (values.size() != expected) {
        return QStringLiteral("%1 takes %2 values").arg(spec.kind).arg(expected);
    }
    int slot = 0;
    for (const FilterParamSpec& param : spec.params) {
        for (int i = 0; i < param.slotCount; ++i, ++slot) {
            if (!std::isfinite(values.at(slot))) {
                return QStringLiteral("%1 must be finite").arg(param.label);
            }
            if (const QString problem = slotProblem(param, values.at(slot)); !problem.isEmpty()) {
                return problem;
            }
        }
    }
    return {};
}

// Dialog-free replays an agent may run, with the keys each may carry. Default
// deny: an unlisted key could steer an executor, as `in` does on Save.
const QHash<QString, QStringList>& runnableCommands()
{
    static const QHash<QString, QStringList> table = {
        {QStringLiteral("image.image.size"),
         {QStringLiteral("width"), QStringLiteral("height"), QStringLiteral("constrainProportions"),
          QStringLiteral("resolution"), QStringLiteral("perCm"), QStringLiteral("resample")}},
        {QStringLiteral("image.canvas.size"),
         {QStringLiteral("width"), QStringLiteral("height"), QStringLiteral("anchor"),
          QStringLiteral("color")}},
        {QStringLiteral("image.rotate.arbitrary"), {QStringLiteral("angle")}},
        {QStringLiteral("edit.fill"),
         {QStringLiteral("color"), QStringLiteral("mode"), QStringLiteral("opacity"),
          QStringLiteral("preserveTransparency")}},
        {QStringLiteral("image.mode.rgb.color"), {}},
        {QStringLiteral("image.mode.grayscale"), {}},
        {QStringLiteral("image.mode.8.bits"), {}},
        {QStringLiteral("image.mode.16.bits"), {}},
        {QStringLiteral("layer.duplicate.layer"), {}},
        {QStringLiteral("layer.flatten.image"), {}},
    };
    return table;
}

// The size `image.image.size` or `image.canvas.size` would make, mirroring the
// Image Size executor's constrained side, within ImageBudget.
bool resizeFitsBudget(PictureView& view, const QJsonObject& values)
{
    int width = values.value(QStringLiteral("width")).toInt();
    int height = values.value(QStringLiteral("height")).toInt();
    if (width <= 0 && height > 0) {
        width = qRound(double(view.document_width()) * height / view.document_height());
    } else if (height <= 0 && width > 0) {
        height = qRound(double(view.document_height()) * width / view.document_width());
    }
    return document_budget_allows(width, height, document_api::documentDepthBits(view));
}

} // namespace

QStringList ControlServer::adjustmentKinds()
{
    return {QStringLiteral("levels"),          QStringLiteral("curves"),
            QStringLiteral("brightness-contrast"), QStringLiteral("hue-saturation"),
            QStringLiteral("color-balance"),   QStringLiteral("exposure"),
            QStringLiteral("vibrance"),        QStringLiteral("black-white"),
            QStringLiteral("photo-filter"),    QStringLiteral("channel-mixer"),
            QStringLiteral("selective-color"), QStringLiteral("gradient-map"),
            QStringLiteral("posterize"),       QStringLiteral("threshold")};
}

QJsonObject ControlServer::applyFilterWithParams(const QString& kind, const QJsonValue& values)
{
    const FilterCommandSpec* spec = filterCommandForKind(kind);
    if (!spec) {
        return error(QStringLiteral("invalid_param"),
                     QStringLiteral("unknown filter kind: %1").arg(kind));
    }
    if (needsResource(*spec)) {
        return error(QStringLiteral("refused"),
                     QStringLiteral("%1 needs its own dialog").arg(kind));
    }
    if (!values.isArray()) {
        return error(QStringLiteral("invalid_param"), QStringLiteral("params must be an array"));
    }
    QList<double> numbers;
    for (const QJsonValue& value : values.toArray()) {
        if (!value.isDouble()) {
            return error(QStringLiteral("invalid_param"),
                         QStringLiteral("params must be numbers"));
        }
        numbers.append(value.toDouble());
    }
    if (const QString problem = filterValuesProblem(*spec, numbers); !problem.isEmpty()) {
        return error(QStringLiteral("invalid_param"), problem);
    }
    if (!apply_filter_params(*frame_->activeView(), kind, numbers)) {
        return error(QStringLiteral("refused"), QStringLiteral("the filter did not apply"));
    }
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("kind"), kind);
    return ok(result);
}

QJsonObject ControlServer::methodListFilters(const QJsonObject&)
{
    QJsonArray filters;
    for (const FilterCommandSpec& spec : filterCommands()) {
        QJsonArray parameters;
        for (const FilterParamSpec& param : spec.params) {
            parameters.append(QJsonObject{{QStringLiteral("label"), param.label},
                                          {QStringLiteral("minimum"), param.minimum},
                                          {QStringLiteral("maximum"), param.maximum},
                                          {QStringLiteral("default"), param.value},
                                          {QStringLiteral("slots"), param.slotCount},
                                          {QStringLiteral("choices"),
                                           QJsonArray::fromStringList(param.choices)}});
        }
        filters.append(QJsonObject{{QStringLiteral("kind"), spec.kind},
                                   {QStringLiteral("label"), spec.label},
                                   {QStringLiteral("available"), !needsResource(spec)},
                                   {QStringLiteral("params"), parameters}});
    }
    QJsonObject result;
    result.insert(QStringLiteral("filters"), filters);
    result.insert(QStringLiteral("count"), filters.size());
    return ok(result);
}

QJsonObject ControlServer::methodListAdjustments(const QJsonObject&)
{
    QJsonArray adjustments;
    for (const QString& kind : adjustmentKinds()) {
        const QByteArray block = bytes(image_adjustment_default(kind, 0x000000u, 0xffffffu));
        const QStringList page = image_adjustment_page(slice(block));
        adjustments.append(QJsonObject{{QStringLiteral("kind"), kind},
                                       {QStringLiteral("page"), QJsonArray::fromStringList(page)}});
    }
    QJsonObject result;
    result.insert(QStringLiteral("adjustments"), adjustments);
    return ok(result);
}

QJsonObject ControlServer::methodAdjustApply(const QJsonObject& params)
{
    PictureView* view = frame_->activeView();
    if (!view || !view->has_document()) {
        return error(QStringLiteral("no_document"), QStringLiteral("no active document"));
    }
    const QString kind = params.value(QStringLiteral("kind")).toString();
    if (!adjustmentKinds().contains(kind)) {
        return error(QStringLiteral("invalid_param"),
                     QStringLiteral("unknown adjustment kind: %1").arg(kind));
    }
    QByteArray block = bytes(image_adjustment_default(kind, 0x000000u, 0xffffffu));
    const QJsonObject values = params.value(QStringLiteral("values")).toObject();
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (!it.value().isDouble()) {
            return error(QStringLiteral("invalid_param"),
                         QStringLiteral("%1 must be a number").arg(it.key()));
        }
        block = bytes(image_adjustment_set(slice(block), it.key(), it.value().toDouble()));
        if (block.isEmpty()) {
            return error(QStringLiteral("invalid_param"),
                         QStringLiteral("refused value for %1").arg(it.key()));
        }
    }
    if (!image_adjust_apply(*view, slice(block), kind)) {
        return error(QStringLiteral("refused"), QStringLiteral("the adjustment did not apply"));
    }
    return ok({{QStringLiteral("kind"), kind}});
}

QJsonObject ControlServer::methodRunCommand(const QJsonObject& params)
{
    const QString id = params.value(QStringLiteral("id")).toString();
    const auto row = runnableCommands().constFind(id);
    if (row == runnableCommands().constEnd()) {
        return error(QStringLiteral("refused"),
                     QStringLiteral("command cannot be run with parameters: %1").arg(id));
    }
    PictureView* view = frame_->activeView();
    if (!view || !view->has_document()) {
        return error(QStringLiteral("no_document"), QStringLiteral("no active document"));
    }
    // KookaPictura-SHP fork: the registry's blocked state is not checked by
    // executeRecorded, so reshaping an SHP sheet is refused here.
    if (ShpSupport::blocks(frame_, id)) {
        return error(QStringLiteral("refused"), ShpSupport::blockedReason());
    }
    const QJsonObject values = params.value(QStringLiteral("params")).toObject();
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (!row->contains(it.key())) {
            return error(QStringLiteral("invalid_param"),
                         QStringLiteral("%1 does not take %2").arg(id, it.key()));
        }
    }
    const bool resizes = id == QStringLiteral("image.image.size")
        || id == QStringLiteral("image.canvas.size");
    if (resizes && !resizeFitsBudget(*view, values)) {
        return error(QStringLiteral("invalid_param"),
                     QStringLiteral("that size exceeds the image budget"));
    }
    CommandRegistry* registry = frame_->registry();
    CommandParams command;
    command.values = values;
    // Empty means "keep the pixels", which refuses any change of size. The
    // dialog opens on Bicubic, and that is what someone naming a size wants.
    if (id == QStringLiteral("image.image.size")
        && !values.contains(QStringLiteral("resample"))) {
        command.values.insert(QStringLiteral("resample"), QStringLiteral("bicubic"));
    }
    if (!registry->executeRecorded(id, command)) {
        return error(QStringLiteral("refused"),
                     QStringLiteral("command did not run: %1").arg(id));
    }
    registry->refresh();
    return ok({{QStringLiteral("id"), id}});
}

QJsonObject ControlServer::methodLayerTree(const QJsonObject&)
{
    PictureView* view = frame_->activeView();
    if (!view || !view->has_document()) {
        return error(QStringLiteral("no_document"), QStringLiteral("no active document"));
    }
    QJsonArray layers;
    for (const auto& row : view->layers_snapshot(0, false)) {
        layers.append(QJsonObject{{QStringLiteral("path"), row.path},
                                  {QStringLiteral("depth"), row.depth},
                                  {QStringLiteral("name"), row.name},
                                  {QStringLiteral("kind"), row.kind},
                                  {QStringLiteral("visible"), row.visible},
                                  {QStringLiteral("opacity"), row.opacity},
                                  {QStringLiteral("blend"), row.blend}});
    }
    QJsonObject result;
    result.insert(QStringLiteral("layers"), layers);
    result.insert(QStringLiteral("active"), view->active_layer_path());
    return ok(result);
}

QJsonObject ControlServer::methodSetActiveLayer(const QJsonObject& params)
{
    PictureView* view = frame_->activeView();
    if (!view || !view->has_document()) {
        return error(QStringLiteral("no_document"), QStringLiteral("no active document"));
    }
    const QString path = params.value(QStringLiteral("path")).toString();
    if (layer_kind_path(*view, path).isEmpty()) {
        return error(QStringLiteral("invalid_param"),
                     QStringLiteral("no layer at path: %1").arg(path));
    }
    view->set_active_layer(path);
    frame_->refresh();
    return ok({{QStringLiteral("active"), view->active_layer_path()}});
}

QJsonObject ControlServer::methodExportPng(const QJsonObject& params)
{
    PictureView* view = frame_->activeView();
    if (!view || !view->has_document()) {
        return error(QStringLiteral("no_document"), QStringLiteral("no active document"));
    }
    const QString path = params.value(QStringLiteral("path")).toString();
    if (path.isEmpty()) {
        return error(QStringLiteral("invalid_param"), QStringLiteral("path is required"));
    }
    if (const QString problem = pathProblem(path, true); !problem.isEmpty()) {
        return error(QStringLiteral("invalid_param"), problem);
    }
    if (!export_image(*view, path, QStringLiteral("PNG"), -1, 100)) {
        return error(QStringLiteral("io_error"), QStringLiteral("export failed"));
    }
    return ok({{QStringLiteral("path"), path}});
}

QJsonObject ControlServer::methodHistory(const QJsonObject& params)
{
    PictureView* view = frame_->activeView();
    if (!view || !view->has_document()) {
        return error(QStringLiteral("no_document"), QStringLiteral("no active document"));
    }
    const QString op = params.value(QStringLiteral("op")).toString();
    bool applied = true;
    if (op == QStringLiteral("jump")) {
        const int index = params.value(QStringLiteral("index")).toInt(-1);
        if (index < 0 || index >= history_api::count(*view)) {
            return error(QStringLiteral("invalid_param"), QStringLiteral("no such state"));
        }
        applied = history_api::jump(*view, index);
    } else if (op == QStringLiteral("snapshot")) {
        const QString label = params.value(QStringLiteral("label")).toString();
        if (label.isEmpty() || label.size() > kMaxNameChars) {
            return error(QStringLiteral("invalid_param"),
                         QStringLiteral("label must be 1..%1 characters").arg(kMaxNameChars));
        }
        applied = history_api::addSnapshot(*view, label);
    } else if (op == QStringLiteral("restore")) {
        const int index = params.value(QStringLiteral("index")).toInt(-1);
        if (index < 0 || index >= history_api::snapshotCount(*view)) {
            return error(QStringLiteral("invalid_param"), QStringLiteral("no such snapshot"));
        }
        applied = history_api::restoreSnapshot(*view, index);
    } else if (op != QStringLiteral("list")) {
        return error(QStringLiteral("invalid_param"), QStringLiteral("unknown op: %1").arg(op));
    }
    if (!applied) {
        return error(QStringLiteral("refused"), QStringLiteral("history op did not apply"));
    }
    frame_->refresh();

    QJsonArray states;
    for (int i = 0; i < history_api::count(*view); ++i) {
        states.append(history_api::label(*view, i));
    }
    QJsonArray snapshots;
    for (int i = 0; i < history_api::snapshotCount(*view); ++i) {
        snapshots.append(history_api::snapshotLabel(*view, i));
    }
    QJsonObject result;
    result.insert(QStringLiteral("states"), states);
    result.insert(QStringLiteral("snapshots"), snapshots);
    result.insert(QStringLiteral("index"), history_api::index(*view));
    return ok(result);
}

QJsonObject ControlServer::methodCommitCrop(const QJsonObject&)
{
    if (!frame_->hasPendingCrop()) {
        return error(QStringLiteral("refused"), QStringLiteral("no crop is pending"));
    }
    if (policy_.live && !agentCrop_) {
        return error(QStringLiteral("human_busy"),
                     QStringLiteral("waiting for the person: the crop is theirs"));
    }
    if (!frame_->commitCrop()) {
        return error(QStringLiteral("refused"), QStringLiteral("the crop did not apply"));
    }
    return ok({{QStringLiteral("committed"), true}});
}

} // namespace pictura
