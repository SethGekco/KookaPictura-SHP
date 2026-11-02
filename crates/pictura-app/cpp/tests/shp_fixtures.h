// SPDX-License-Identifier: GPL-3.0-or-later
// KookaPictura-SHP fork: generated sprites and palettes shared by the SHP Qt
// tests. No game file is used.
#pragma once

#include "pictura_app/src/cxxqt_object.cxxqt.h"

#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QString>

namespace {

constexpr int kW = 6;
constexpr int kH = 5;
constexpr int kFrames = 10;

inline void put16(QByteArray& out, int v)
{
    out.append(char(v & 0xFF));
    out.append(char((v >> 8) & 0xFF));
}

inline void put32(QByteArray& out, quint32 v)
{
    put16(out, int(v & 0xFFFF));
    put16(out, int(v >> 16));
}

// Frame k: index 40 + k everywhere except a transparent corner, stored raw
// on the full canvas.
inline QByteArray sprite()
{
    QByteArray out;
    put16(out, 0);
    put16(out, kW);
    put16(out, kH);
    put16(out, kFrames);
    const int base = 8 + 24 * kFrames;
    for (int k = 0; k < kFrames; ++k) {
        put16(out, 0);
        put16(out, 0);
        put16(out, kW);
        put16(out, kH);
        put32(out, 0); // raw
        put32(out, 0x00112233u);
        put32(out, 0);
        put32(out, quint32(base + k * kW * kH));
    }
    for (int k = 0; k < kFrames; ++k) {
        for (int i = 0; i < kW * kH; ++i) {
            out.append(char(i == 0 ? 0 : 40 + k));
        }
    }
    return out;
}

// A 6-bit VGA palette that repeats each colour on four indices, as game
// palettes repeat colours.
inline QByteArray palette()
{
    QByteArray out;
    for (int i = 0; i < 256; ++i) {
        const int v = (i / 4) % 64;
        out.append(char(v));
        out.append(char(63 - v));
        out.append(char(v / 2));
    }
    return out;
}

inline bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

inline QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// One hard Pencil dab of `argb` on the pixel (x, y) of the sheet.
inline bool dab(pictura::PictureView* view, quint32 argb, int x, int y)
{
    view->set_active_layer(QStringLiteral("0"));
    return view->begin_paint(argb, 0xFFFFFFFFu, 1, 100, 100, 0, 100, 100, 25,
                             QStringLiteral("normal"), true, false)
        && view->paint_dab(x + 0.5, y + 0.5, 1.0) && view->end_paint();
}

} // namespace
