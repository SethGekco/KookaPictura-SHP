// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: the playback preview of an SHP document.

#include "panels/shp_playback_panel.h"

#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

#include "pictura_app/src/cxxqt_object.cxxqt.h"
#include "pictura_app/src/cxxqt_object/shp.cxxqt.h"

namespace pictura {

ShpPlaybackPanel::ShpPlaybackPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("shpPlaybackPanel"));
    cache_.setMaxCost(kCacheBytes);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);
    screen_ = new QLabel(this);
    screen_->setObjectName(QStringLiteral("shpPlaybackView"));
    screen_->setAlignment(Qt::AlignCenter);
    screen_->setMinimumSize(96, 96);
    screen_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(screen_, 1);

    auto* row = new QHBoxLayout;
    play_ = new QToolButton(this);
    play_->setObjectName(QStringLiteral("shpPlay"));
    play_->setCheckable(true);
    play_->setText(tr("Play"));
    play_->setToolTip(tr("Play or pause the preview"));
    label_ = new QLabel(this);
    label_->setObjectName(QStringLiteral("shpPlaybackFrame"));
    row->addWidget(play_);
    row->addWidget(label_, 1);
    layout->addLayout(row);

    auto* options = new QHBoxLayout;
    fps_ = new QSpinBox(this);
    fps_->setObjectName(QStringLiteral("shpPlaybackFps"));
    fps_->setRange(1, 60);
    fps_->setValue(15);
    fps_->setSuffix(tr(" fps"));
    from_ = new QSpinBox(this);
    from_->setObjectName(QStringLiteral("shpPlaybackFrom"));
    from_->setToolTip(tr("First frame played"));
    to_ = new QSpinBox(this);
    to_->setObjectName(QStringLiteral("shpPlaybackTo"));
    to_->setToolTip(tr("Last frame played"));
    loop_ = new QCheckBox(tr("Loop"), this);
    loop_->setObjectName(QStringLiteral("shpPlaybackLoop"));
    loop_->setChecked(true);
    options->addWidget(fps_);
    options->addWidget(from_);
    options->addWidget(new QLabel(QStringLiteral("-"), this));
    options->addWidget(to_);
    options->addWidget(loop_);
    options->addStretch(1);
    layout->addLayout(options);

    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(1000 / fps_->value());
    connect(&timer_, &QTimer::timeout, this, &ShpPlaybackPanel::advance);
    connect(play_, &QToolButton::toggled, this, [this](bool on) { setPlaying(on); });
    connect(fps_, &QSpinBox::valueChanged, this,
            [this](int fps) { timer_.setInterval(std::max(1, 1000 / fps)); });
    connect(from_, &QSpinBox::valueChanged, this, [this](int from) {
        if (to_->value() < from) {
            to_->setValue(from);
        }
        if (frame_ < from || frame_ > to_->value()) {
            showFrame(from);
        }
    });
    connect(to_, &QSpinBox::valueChanged, this, [this](int to) {
        if (from_->value() > to) {
            from_->setValue(to);
        }
        if (frame_ < from_->value() || frame_ > to) {
            showFrame(from_->value());
        }
    });
    syncControls();
}

void ShpPlaybackPanel::setView(PictureView* view)
{
    if (view_ != view) {
        setPlaying(false);
        cache_.clear();
        total_ = 0;
        frame_ = 0;
        generation_ = 0;
    }
    view_ = view;
    refresh();
}

void ShpPlaybackPanel::refresh()
{
    const ShpInfo info = view_ ? shp_info(*view_) : ShpInfo{};
    const int total = info.valid ? info.total : 0;
    const QString palette = info.palette_path + QLatin1Char('|') + info.palette_name;
    const quint64 generation = view_ ? view_->document_generation() : 0;
    if (palette != palette_ || generation != generation_) {
        cache_.clear();
        palette_ = palette;
        generation_ = generation;
    }
    if (total != total_) {
        const bool wholeRange = total_ == 0 || to_->value() >= total_ - 1;
        total_ = total;
        const QSignalBlocker blockFrom(from_);
        const QSignalBlocker blockTo(to_);
        from_->setRange(0, std::max(0, total - 1));
        to_->setRange(0, std::max(0, total - 1));
        if (wholeRange) {
            to_->setValue(std::max(0, total - 1));
        }
        if (from_->value() > to_->value()) {
            from_->setValue(to_->value());
        }
    }
    if (total == 0) {
        setPlaying(false);
    }
    syncControls();
    showFrame(std::clamp(frame_, from_->value(), to_->value()));
}

void ShpPlaybackPanel::syncControls()
{
    const bool on = total_ > 0;
    for (QWidget* w : {static_cast<QWidget*>(play_), static_cast<QWidget*>(fps_),
                       static_cast<QWidget*>(from_), static_cast<QWidget*>(to_),
                       static_cast<QWidget*>(loop_)}) {
        w->setEnabled(on);
    }
}

bool ShpPlaybackPanel::playing() const
{
    return timer_.isActive();
}

void ShpPlaybackPanel::setPlaying(bool on)
{
    on = on && total_ > 0;
    if (on && !loop_->isChecked() && frame_ >= to_->value()) {
        // Played to the end once: play again from the start.
        showFrame(from_->value());
    }
    if (on) {
        timer_.start();
    } else {
        timer_.stop();
    }
    const QSignalBlocker block(play_);
    play_->setChecked(on);
    play_->setText(on ? tr("Pause") : tr("Play"));
}

void ShpPlaybackPanel::advance()
{
    if (total_ <= 0) {
        setPlaying(false);
        return;
    }
    const int from = from_->value();
    const int to = to_->value();
    int next = frame_ + 1;
    if (next > to || next < from) {
        if (loop_->isChecked()) {
            next = from;
        } else {
            setPlaying(false);
            return;
        }
    }
    showFrame(next);
    if (!loop_->isChecked() && next >= to) {
        setPlaying(false);
    }
}

void ShpPlaybackPanel::setFrame(int frame)
{
    showFrame(std::clamp(frame, 0, std::max(0, total_ - 1)));
}

void ShpPlaybackPanel::setRange(int from, int to)
{
    to_->setValue(to);
    from_->setValue(from);
}

int ShpPlaybackPanel::from() const
{
    return from_->value();
}

int ShpPlaybackPanel::to() const
{
    return to_->value();
}

void ShpPlaybackPanel::setFps(int fps)
{
    fps_->setValue(fps);
}

int ShpPlaybackPanel::fps() const
{
    return fps_->value();
}

void ShpPlaybackPanel::setLoop(bool on)
{
    loop_->setChecked(on);
}

QImage ShpPlaybackPanel::frameImage(int index)
{
    if (!view_ || index < 0 || index >= total_) {
        return {};
    }
    const ShpInfo info = shp_info(*view_);
    const int cell = index - info.first;
    if (cell >= 0 && cell < info.frames && info.cols > 0) {
        // On the sheet: the live pixels, so edits show at once.
        return view_->display_image(0, (cell % info.cols) * (info.width + info.gutter),
                                    (cell / info.cols) * (info.height + info.gutter),
                                    info.width, info.height);
    }
    const quint64 stamp = shp_frame_stamp(*view_, index);
    if (const Cached* hit = cache_.object(index); hit && hit->stamp == stamp) {
        return hit->image;
    }
    const ShpThumb t = shp_frame_thumbnail(*view_, index, std::max(info.width, info.height));
    if (t.width <= 0 || t.height <= 0 || t.argb.size() != size_t(t.width) * size_t(t.height)) {
        return {};
    }
    QImage image(t.width, t.height, QImage::Format_ARGB32);
    for (int y = 0; y < t.height; ++y) {
        std::copy_n(t.argb.data() + size_t(y) * size_t(t.width), t.width,
                    reinterpret_cast<uint32_t*>(image.scanLine(y)));
    }
    const qint64 cost = qint64(image.sizeInBytes());
    if (cost <= kCacheBytes) {
        cache_.insert(index, new Cached{stamp, image}, cost);
    }
    return image;
}

void ShpPlaybackPanel::showFrame(int frame)
{
    frame_ = frame;
    if (total_ <= 0) {
        screen_->clear();
        label_->setText(tr("No frames"));
        return;
    }
    label_->setText(tr("Frame %1").arg(frame));
    const QImage image = frameImage(frame);
    if (image.isNull()) {
        screen_->clear();
        return;
    }
    // Whole-number zoom (1 to 8) so pixels stay square.
    const QSize room = screen_->size().boundedTo(QSize(1024, 1024));
    const int zoom = std::clamp(std::min(room.width() / std::max(1, image.width()),
                                         room.height() / std::max(1, image.height())),
                                1, 8);
    QPixmap pixmap(image.size() * zoom);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.drawImage(QRect(QPoint(0, 0), pixmap.size()), image);
    p.end();
    screen_->setPixmap(pixmap);
}

} // namespace pictura

#include "moc_shp_playback_panel.cpp"
