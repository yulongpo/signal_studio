#pragma once

#include "domain/project.h"

#include <QImage>
#include <QList>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace signalstudio::chart_palette {

inline QRgb color(float level, Palette palette) {
    static const std::vector<std::array<int, 3>> turbo{{8,14,52},{20,58,141},{10,144,216},{22,207,222},{40,225,138},{224,229,70},{250,159,52},{188,49,52}};
    static const std::vector<std::array<int, 3>> viridis{{40,18,71},{62,71,129},{49,112,142},{37,154,141},{89,189,94},{206,219,53}};
    static const std::vector<std::array<int, 3>> gray{{11,17,26},{62,75,95},{133,149,159},{220,228,234}};
    static const std::vector<std::array<int, 3>> plasma{{13,8,135},{126,3,168},{204,71,120},{248,149,64},{240,249,33}};
    static const std::vector<std::array<int, 3>> inferno{{0,0,4},{87,16,110},{188,55,84},{249,142,9},{252,255,164}};
    static const std::vector<std::array<int, 3>> magma{{0,0,4},{81,18,124},{183,55,121},{251,136,97},{252,253,191}};
    static const std::vector<std::array<int, 3>> cividis{{0,32,76},{59,82,139},{124,123,120},{188,167,72},{253,231,55}};
    static const std::vector<std::array<int, 3>> coolEditClassic{
        {11,4,41}, {22,2,64}, {43,0,98}, {105,0,113},
        {161,0,96}, {215,12,45}, {245,66,11}, {255,175,28}, {255,234,46}
    };
    const auto& stops = [&]() -> const std::vector<std::array<int, 3>>& {
        switch (palette) {
        case Palette::Turbo: return turbo;
        case Palette::Viridis: return viridis;
        case Palette::Gray: return gray;
        case Palette::Plasma: return plasma;
        case Palette::Inferno: return inferno;
        case Palette::Magma: return magma;
        case Palette::Cividis: return cividis;
        case Palette::CoolEditClassic: return coolEditClassic;
        }
        return turbo;
    }();
    const double z = std::clamp(static_cast<double>(level), 0.0, .999) * (stops.size() - 1);
    const auto index = static_cast<std::size_t>(z);
    const double ratio = z - index;
    const auto channel = [&](int i) { return static_cast<int>(std::lround(stops[index][i] * (1 - ratio) + stops[index + 1][i] * ratio)); };
    return qRgb(channel(0), channel(1), channel(2));
}

inline QImage texture(Palette palette) {
    static const std::array<QImage, 8> palettes = [] {
        std::array<QImage, 8> images;
        for (int index = 0; index < static_cast<int>(images.size()); ++index) {
            auto& image = images[static_cast<std::size_t>(index)];
            image = QImage(256, 1, QImage::Format_RGBA8888);
            auto* row = image.scanLine(0);
            for (int x = 0; x < 256; ++x) {
                const auto rgb = color(static_cast<float>(x) / 255.0f, static_cast<Palette>(index));
                row[x * 4] = static_cast<uchar>(qRed(rgb));
                row[x * 4 + 1] = static_cast<uchar>(qGreen(rgb));
                row[x * 4 + 2] = static_cast<uchar>(qBlue(rgb));
                row[x * 4 + 3] = 255;
            }
        }
        return images;
    }();
    const auto index = std::clamp(static_cast<int>(palette), 0, static_cast<int>(palettes.size()) - 1);
    return palettes[static_cast<std::size_t>(index)];
}

inline QList<QRgb> colorTable(Palette palette) {
    static const std::array<QList<QRgb>, 8> tables = [] {
        std::array<QList<QRgb>, 8> values;
        for (int index = 0; index < static_cast<int>(values.size()); ++index) {
            auto& table = values[static_cast<std::size_t>(index)];
            table.reserve(256);
            for (int x = 0; x < 256; ++x)
                table.push_back(color(static_cast<float>(x) / 255.0f, static_cast<Palette>(index)));
        }
        return values;
    }();
    const auto index = std::clamp(static_cast<int>(palette), 0, static_cast<int>(tables.size()) - 1);
    return tables[static_cast<std::size_t>(index)];
}

} // namespace signalstudio::chart_palette
