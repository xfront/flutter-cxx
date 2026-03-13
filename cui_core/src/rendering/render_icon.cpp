/// @file rendering/render_icon.cpp
/// @brief 图标路径构建与绘制。所有图标在 24×24 网格内描述。

#include "cui/rendering/render_icon.h"

#include <cmath>
#include <numbers>

#include <include/core/SkCanvas.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkRRect.h>

#include "cui/rendering/painting_context.h"

namespace cui {
namespace {

SkRect squareRect(double cx, double cy, double r) {
    return SkRect::MakeLTRB(SkScalar(cx - r), SkScalar(cy - r), SkScalar(cx + r),
                            SkScalar(cy + r));
}

/// 图标数据：填充路径 + 描边路径（24×24 网格坐标）。
struct IconPaths {
    SkPath fill;
    SkPath stroke;
};

/// 按图标编号构建 24×24 网格内的路径。
IconPaths buildIconPaths(IconCode code) {
    IconPaths ip;
    SkPathBuilder f, s;
    switch (code) {
        case IconCode::Check:
            s.moveTo(4, 12);
            s.lineTo(10, 18);
            s.lineTo(20, 6);
            break;
        case IconCode::Close:
            s.moveTo(6, 6);
            s.lineTo(18, 18);
            s.moveTo(18, 6);
            s.lineTo(6, 18);
            break;
        case IconCode::Add:
            s.moveTo(12, 5);
            s.lineTo(12, 19);
            s.moveTo(5, 12);
            s.lineTo(19, 12);
            break;
        case IconCode::Remove:
            s.moveTo(5, 12);
            s.lineTo(19, 12);
            break;
        case IconCode::ArrowBack:
            s.moveTo(20, 12);
            s.lineTo(4, 12);
            s.moveTo(10, 6);
            s.lineTo(4, 12);
            s.lineTo(10, 18);
            break;
        case IconCode::ArrowForward:
            s.moveTo(4, 12);
            s.lineTo(20, 12);
            s.moveTo(14, 6);
            s.lineTo(20, 12);
            s.lineTo(14, 18);
            break;
        case IconCode::ArrowUpward:
            s.moveTo(12, 20);
            s.lineTo(12, 4);
            s.moveTo(5, 11);
            s.lineTo(12, 4);
            s.lineTo(19, 11);
            break;
        case IconCode::ArrowDownward:
            s.moveTo(12, 4);
            s.lineTo(12, 20);
            s.moveTo(5, 13);
            s.lineTo(12, 20);
            s.lineTo(19, 13);
            break;
        case IconCode::ExpandMore:
            s.moveTo(7.5, 9.5);
            s.lineTo(12, 14);
            s.lineTo(16.5, 9.5);
            break;
        case IconCode::Menu:
            s.moveTo(4, 6);
            s.lineTo(20, 6);
            s.moveTo(4, 12);
            s.lineTo(20, 12);
            s.moveTo(4, 18);
            s.lineTo(20, 18);
            break;
        case IconCode::Search:
            s.addOval(squareRect(10.5, 10.5, 6.5));
            s.moveTo(15.5, 15.5);
            s.lineTo(20.5, 20.5);
            break;
        case IconCode::Home:
            s.moveTo(4, 11);
            s.lineTo(12, 4);
            s.lineTo(20, 11);
            s.moveTo(6, 10);
            s.lineTo(6, 20);
            s.lineTo(18, 20);
            s.lineTo(18, 10);
            break;
        case IconCode::Favorite:
            // 经典心形（Material favorite 的贝塞尔近似）。
            f.moveTo(12, 21);
            f.lineTo(10.6, 19.7);
            f.cubicTo(5.4, 15.4, 2, 12.3, 2, 8.5);
            f.cubicTo(2, 5.4, 4.4, 3, 7.5, 3);
            f.cubicTo(9.2, 3, 10.9, 3.8, 12, 5.1);
            f.cubicTo(13.1, 3.8, 14.8, 3, 16.5, 3);
            f.cubicTo(19.6, 3, 22, 5.4, 22, 8.5);
            f.cubicTo(22, 12.3, 18.6, 15.4, 13.4, 19.7);
            f.close();
            break;
        case IconCode::Star: {
            // 五角星：外接圆 r=10、内切圆 r=4.2，尖朝上。
            const double cx = 12.0, cy = 12.8, ro = 10.0, ri = 4.2;
            for (int i = 0; i < 10; ++i) {
                double ang = -std::numbers::pi / 2.0 + i * std::numbers::pi / 5.0;
                double r = (i % 2 == 0) ? ro : ri;
                double x = cx + r * std::cos(ang);
                double y = cy + r * std::sin(ang);
                if (i == 0)
                    f.moveTo(SkScalar(x), SkScalar(y));
                else
                    f.lineTo(SkScalar(x), SkScalar(y));
            }
            f.close();
            break;
        }
        case IconCode::Play:
            f.moveTo(8, 5);
            f.lineTo(8, 19);
            f.lineTo(19, 12);
            f.close();
            break;
        case IconCode::Pause:
            f.moveTo(7, 5);
            f.lineTo(10, 5);
            f.lineTo(10, 19);
            f.lineTo(7, 19);
            f.close();
            f.moveTo(14, 5);
            f.lineTo(17, 5);
            f.lineTo(17, 19);
            f.lineTo(14, 19);
            f.close();
            break;
        case IconCode::MoreVert:
            f.addOval(squareRect(12, 5, 2));
            f.addOval(squareRect(12, 12, 2));
            f.addOval(squareRect(12, 19, 2));
            break;
        case IconCode::MoreHoriz:
            f.addOval(squareRect(5, 12, 2));
            f.addOval(squareRect(12, 12, 2));
            f.addOval(squareRect(19, 12, 2));
            break;
        case IconCode::Delete:
            s.moveTo(5, 7);
            s.lineTo(19, 7);
            s.moveTo(9, 7);
            s.lineTo(9, 5);
            s.lineTo(15, 5);
            s.lineTo(15, 7);
            s.moveTo(6.5, 7);
            s.lineTo(7.5, 20);
            s.lineTo(16.5, 20);
            s.lineTo(17.5, 7);
            s.moveTo(10, 11);
            s.lineTo(10, 16);
            s.moveTo(14, 11);
            s.lineTo(14, 16);
            break;
        case IconCode::Person:
            f.addOval(squareRect(12, 8, 4));
            f.moveTo(4, 20);
            f.cubicTo(4, 15.9, 7.6, 14, 12, 14);
            f.cubicTo(16.4, 14, 20, 15.9, 20, 20);
            f.lineTo(20, 21);
            f.lineTo(4, 21);
            f.close();
            break;
        case IconCode::Settings:
            s.addOval(squareRect(12, 12, 7));
            f.addOval(squareRect(12, 12, 2.5));
            break;
        case IconCode::Button:
            s.addRRect(SkRRect::MakeRectXY(SkRect::MakeLTRB(3, 7, 21, 17), 3, 3));
            break;
        case IconCode::Toggle:
            s.addRRect(SkRRect::MakeRectXY(SkRect::MakeLTRB(3, 8, 21, 16), 4, 4));
            f.addOval(squareRect(16, 12, 3));
            break;
        case IconCode::Radio:
            s.addOval(squareRect(12, 12, 8));
            f.addOval(squareRect(12, 12, 3));
            break;
        case IconCode::Checkbox:
            s.addRRect(SkRRect::MakeRectXY(SkRect::MakeLTRB(4, 4, 20, 20), 2.5, 2.5));
            s.moveTo(7.5, 12);
            s.lineTo(10.5, 15);
            s.lineTo(16.5, 8.5);
            break;
        case IconCode::Dropdown:
            s.addRRect(SkRRect::MakeRectXY(SkRect::MakeLTRB(3, 5, 21, 13), 2, 2));
            s.moveTo(8, 17);
            s.lineTo(12, 21);
            s.lineTo(16, 17);
            break;
        case IconCode::Slider:
            s.moveTo(3, 12);
            s.lineTo(21, 12);
            f.addOval(squareRect(15, 12, 3.5));
            break;
        case IconCode::Text:
            s.moveTo(4, 6);
            s.lineTo(20, 6);
            s.moveTo(12, 6);
            s.lineTo(12, 19);
            break;
        case IconCode::Input:
            s.addRRect(SkRRect::MakeRectXY(SkRect::MakeLTRB(3, 6, 21, 18), 3, 3));
            s.moveTo(7, 13);
            s.lineTo(15, 13);
            break;
        case IconCode::Table:
            s.addRect(SkRect::MakeLTRB(3, 5, 21, 19));
            s.moveTo(3, 12);
            s.lineTo(21, 12);
            s.moveTo(12, 5);
            s.lineTo(12, 19);
            break;
        case IconCode::List:
            f.addOval(squareRect(4.5, 7, 1.6));
            f.addOval(squareRect(4.5, 12, 1.6));
            f.addOval(squareRect(4.5, 17, 1.6));
            s.moveTo(9, 7);
            s.lineTo(20, 7);
            s.moveTo(9, 12);
            s.lineTo(20, 12);
            s.moveTo(9, 17);
            s.lineTo(20, 17);
            break;
        case IconCode::Grid:
            s.addRRect(SkRRect::MakeRectXY(SkRect::MakeLTRB(4, 4, 11, 11), 1.5, 1.5));
            s.addRRect(SkRRect::MakeRectXY(SkRect::MakeLTRB(13, 4, 20, 11), 1.5, 1.5));
            s.addRRect(SkRRect::MakeRectXY(SkRect::MakeLTRB(4, 13, 11, 20), 1.5, 1.5));
            s.addRRect(SkRRect::MakeRectXY(SkRect::MakeLTRB(13, 13, 20, 20), 1.5, 1.5));
            break;
        case IconCode::Chart:
            s.moveTo(4, 4);
            s.lineTo(4, 20);
            s.lineTo(20, 20);
            f.addRect(SkRect::MakeLTRB(7, 12, 11, 19));
            f.addRect(SkRect::MakeLTRB(12.5, 7, 16.5, 19));
            break;
        case IconCode::Image:
            s.addRect(SkRect::MakeLTRB(3, 5, 21, 19));
            f.addOval(squareRect(8, 9.5, 1.8));
            s.moveTo(5, 17);
            s.lineTo(10, 12);
            s.lineTo(14, 16);
            s.lineTo(17, 13);
            s.lineTo(20, 16);
            break;
        case IconCode::Dialog:
            s.addRect(SkRect::MakeLTRB(3, 5, 21, 19));
            s.moveTo(3, 10);
            s.lineTo(21, 10);
            break;
        case IconCode::Bell:
            s.moveTo(6, 17);
            s.lineTo(18, 17);
            s.moveTo(7, 17);
            s.cubicTo(7, 10.5, 9, 6, 12, 6);
            s.cubicTo(15, 6, 17, 10.5, 17, 17);
            s.moveTo(10, 19.5);
            s.lineTo(14, 19.5);
            break;
        case IconCode::Palette:
            s.addOval(squareRect(12, 12, 8.5));
            f.addOval(squareRect(9, 9, 1.8));
            f.addOval(squareRect(15, 9, 1.8));
            f.addOval(squareRect(12, 15.5, 1.8));
            break;
        case IconCode::Layers:
            s.moveTo(12, 3.5);
            s.lineTo(20.5, 8);
            s.lineTo(12, 12.5);
            s.lineTo(3.5, 8);
            s.close();
            s.moveTo(3.5, 12);
            s.lineTo(12, 16.5);
            s.lineTo(20.5, 12);
            s.moveTo(3.5, 16);
            s.lineTo(12, 20.5);
            s.lineTo(20.5, 16);
            break;
        case IconCode::Refresh:
            // 环形箭头：270° 圆弧 + 右上方箭头。
            s.addArc(squareRect(12, 12, 8), 60, 270);
            f.moveTo(17.2, 2.8);
            f.lineTo(21.4, 7.4);
            f.lineTo(15.0, 8.2);
            f.close();
            break;
        case IconCode::Tag:
            s.moveTo(3, 11);
            s.lineTo(11, 3);
            s.lineTo(21, 3);
            s.lineTo(21, 13);
            s.lineTo(13, 21);
            s.close();
            f.addOval(squareRect(16.5, 7.5, 1.8));
            break;
        case IconCode::Gesture:
            // 手势：两根手指 + 手腕弧线。
            s.moveTo(9, 12);
            s.lineTo(9, 5.5);
            s.lineTo(12, 8.5);
            s.lineTo(15, 5.5);
            s.lineTo(15, 12);
            s.moveTo(9, 12);
            s.cubicTo(9, 16, 12, 16.5, 12, 19.5);
            s.cubicTo(12, 21.3, 10, 21.3, 10, 19.5);
            s.lineTo(10, 16);
            break;
        case IconCode::Folder:
            s.moveTo(3, 19);
            s.lineTo(3, 6);
            s.lineTo(9.5, 6);
            s.lineTo(12, 8.5);
            s.lineTo(21, 8.5);
            s.lineTo(21, 19);
            s.close();
            break;
        case IconCode::Clock:
            s.addOval(squareRect(12, 12, 8.5));
            s.moveTo(12, 7);
            s.lineTo(12, 12);
            s.lineTo(16, 14);
            break;
        case IconCode::Edit: {
            // 铅笔：笔尖指向左下，斜杆 + 顶端封口 + 中段的箍线。
            s.moveTo(4, 20);
            s.lineTo(4, 16.8);
            s.lineTo(14.8, 6);
            s.lineTo(18, 9.2);
            s.lineTo(7.2, 20);
            s.close();
            s.moveTo(12.6, 7.8);
            s.lineTo(16.2, 11.4);
            break;
        }
        case IconCode::Save: {
            // 软盘：折角外壳 + 顶部标签槽 + 下方写保护方框。
            s.moveTo(5, 3.5);
            s.lineTo(15, 3.5);
            s.lineTo(18.5, 7);
            s.lineTo(18.5, 19);
            s.lineTo(5, 19);
            s.close();
            s.moveTo(8.5, 3.5);
            s.lineTo(8.5, 8);
            s.lineTo(14, 8);
            s.lineTo(14, 3.5);
            s.moveTo(8, 19);
            s.lineTo(8, 12.5);
            s.lineTo(15.5, 12.5);
            s.lineTo(15.5, 19);
            break;
        }
        case IconCode::Share:
            // 上传/分享：上箭头 + 底部托盘。
            s.moveTo(12, 15.5);
            s.lineTo(12, 4.5);
            s.moveTo(7, 9.5);
            s.lineTo(12, 4.5);
            s.lineTo(17, 9.5);
            s.moveTo(5, 19.5);
            s.lineTo(19, 19.5);
            break;
        case IconCode::PushPin: {
            // 图钉（上游 Icons.push_pin 的简化轮廓）：顶柄 → 立柱 → 外扩的肩 →
            // 底座 → 收到一点的针。五段同向闭合，nonzero 缠绕下自动并成一枚。
            f.moveTo(7.2, 2.6);
            f.lineTo(16.8, 2.6);
            f.lineTo(16.8, 4.4);
            f.lineTo(7.2, 4.4);
            f.close();
            f.moveTo(10.4, 4.4);
            f.lineTo(13.6, 4.4);
            f.lineTo(13.6, 8.2);
            f.lineTo(10.4, 8.2);
            f.close();
            f.moveTo(10.4, 8.2);
            f.lineTo(13.6, 8.2);
            f.lineTo(15.8, 10.6);
            f.lineTo(8.2, 10.6);
            f.close();
            f.moveTo(8.2, 10.6);
            f.lineTo(15.8, 10.6);
            f.lineTo(15.8, 12.4);
            f.lineTo(8.2, 12.4);
            f.close();
            f.moveTo(11.2, 12.4);
            f.lineTo(12.8, 12.4);
            f.lineTo(12.0, 20.6);
            f.close();
            break;
        }
        case IconCode::Stop:
            f.addRect(SkRect::MakeLTRB(7, 7, 17, 17));
            break;
        case IconCode::SkipPrevious:
            f.addRect(SkRect::MakeLTRB(6, 6, 8, 18));
            f.moveTo(18, 6);
            f.lineTo(18, 18);
            f.lineTo(10, 12);
            f.close();
            break;
        case IconCode::SkipNext:
            f.moveTo(6, 6);
            f.lineTo(6, 18);
            f.lineTo(14, 12);
            f.close();
            f.addRect(SkRect::MakeLTRB(16, 6, 18, 18));
            break;
        case IconCode::Rewind:
            f.moveTo(12, 12);
            f.lineTo(20, 6);
            f.lineTo(20, 18);
            f.close();
            f.moveTo(4, 12);
            f.lineTo(12, 6);
            f.lineTo(12, 18);
            f.close();
            break;
        case IconCode::FastForward:
            f.moveTo(4, 6);
            f.lineTo(4, 18);
            f.lineTo(12, 12);
            f.close();
            f.moveTo(12, 6);
            f.lineTo(12, 18);
            f.lineTo(20, 12);
            f.close();
            break;
        case IconCode::VolumeUp:
            f.moveTo(4, 9);
            f.lineTo(8, 9);
            f.lineTo(13, 5);
            f.lineTo(13, 19);
            f.lineTo(8, 15);
            f.lineTo(4, 15);
            f.close();
            s.moveTo(16, 9);
            s.cubicTo(18.5, 10.5, 18.5, 13.5, 16, 15);
            s.moveTo(18.5, 7);
            s.cubicTo(22, 9.5, 22, 14.5, 18.5, 17);
            break;
        case IconCode::VolumeMute:
            f.moveTo(4, 9);
            f.lineTo(8, 9);
            f.lineTo(12, 5);
            f.lineTo(12, 19);
            f.lineTo(8, 15);
            f.lineTo(4, 15);
            f.close();
            s.moveTo(16, 10);
            s.lineTo(21, 15);
            s.moveTo(21, 10);
            s.lineTo(16, 15);
            break;
        case IconCode::PhotoCamera:
            s.addRRect(SkRRect::MakeRectXY(SkRect::MakeLTRB(3, 8, 21, 19), 2, 2));
            s.moveTo(8, 8);
            s.lineTo(9.5, 5.5);
            s.lineTo(14.5, 5.5);
            s.lineTo(16, 8);
            s.addOval(squareRect(12, 13.5, 3.2));
            break;
        case IconCode::None:
            break;
    }
    ip.fill = f.detach();
    ip.stroke = s.detach();
    return ip;
}

}  // namespace

void RenderIcon::performPaint(PaintingContext& context, const Offset& offset) {
    SkCanvas* c = context.canvas();
    if (!c || iconSize_ <= 0 || code_ == IconCode::None)
        return;

    IconPaths ip = buildIconPaths(code_);
    double scale = iconSize_ / 24.0;

    c->save();
    c->translate(SkScalar(offset.dx), SkScalar(offset.dy));
    c->scale(SkScalar(scale), SkScalar(scale));

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor4f(SkColor4f{static_cast<float>(color_.r), static_cast<float>(color_.g),
                               static_cast<float>(color_.b), static_cast<float>(color_.a)},
                     nullptr);
    if (!ip.fill.isEmpty()) {
        paint.setStyle(SkPaint::kFill_Style);
        c->drawPath(ip.fill, paint);
    }
    if (!ip.stroke.isEmpty()) {
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(2.0f);  // 24 网格内的描边宽
        paint.setStrokeCap(SkPaint::kRound_Cap);
        paint.setStrokeJoin(SkPaint::kRound_Join);
        c->drawPath(ip.stroke, paint);
    }
    c->restore();
}

}  // namespace cui_core
