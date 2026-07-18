#pragma once
/// @file animated_demo.h
/// @brief 一个可复用的动画演示控件 AnimDemo（纯控件绘制，不依赖任何 legacy
///        旧控件基类）。
///
/// 它把 上游风格的动画三件套封装起来：
///   - AnimationController 控制器（由本框架的 Ticker 逐帧驱动）
///   - AnimatedBuilder（订阅 progressChanged → 每帧重建子树 → 重绘）
///   - CustomPaint（在 painter 回调里读取动画进度做自绘）
///
/// 每个 demo 只需提供：标题/说明、一个 painter 回调（读取 AnimationController 画当前帧）、
/// 一组按钮（按下时通过 AnimClock 启动动画），以及可选的 onTick（推进额外状态，
/// 例如 AnimatedValue / SpringSimulation）。

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/theme.h"
#include "cui/foundation/ticker.h"
#include "cui/framework/framework.h"
#include "cui/widgets/animated_builder.h"
#include "cui/widgets/animation/animation.h"
#include "cui/widgets/basic.h"
#include "cui/widgets/container.h"
#include "cui/widgets/interactive.h"
#include "cui/widgets/layout.h"
#include "cui/widgets/material.h"
#include "cui/widgets/paint.h"
#include "cui/widgets/text.h"

namespace cui {

/// 动画播放控制（页面级共享）。
///
/// 页面把它作为 shared_ptr 传给每个 demo，demo 的 Ticker 每帧读取它：
/// continuous = true 时动画强制单向循环、播完自动从头再来。
struct AnimPlayback {
    bool continuous = true;  ///< 连续播放（不停）  // TEMP-MEASURE
};

/// 动画时钟：由 AnimDemoState 在 initState 中填充，供各 demo 的按钮回调驱动。
struct AnimClock {
    SharedPtr<AnimationController> anim;        ///< 统一的进度时钟
    std::function<void()> run;              ///< 启动动画 + 启动 Ticker（重启）
    std::function<void()> halt;             ///< 停止 Ticker（保留动画当前值）
    std::function<void()> resume;           ///< 仅重启 Ticker
};

/// 一个动画 demo 的描述。
struct AnimDemoSpec {
    std::string title;
    std::string caption;
    SharedPtr<AnimClock> clk;  ///< 由调用方 new 出来，AnimDemoState 填充 anim/run/halt/resume
    /// 绘制当前帧。progress 由时钟给出（已应用 easing）；需要 rawProgress / elapsed
    /// 或额外状态（AnimatedValue / SpringSimulation）时，从捕获的变量里取。
    std::function<void(Painter&, const Size&, AnimationController&)> paint;
    /// 可选：逐帧推进额外状态（如 AnimatedValue::tick）。返回是否继续驱动 Ticker。
    std::function<bool(Duration)> onTick;
    /// 触发按钮：label + 回调（通常调用 clk->run() 或 clk->anim->pause() 等）。
    std::vector<std::pair<std::string, std::function<void()>>> buttons;
    double durationMs = 1500.0;
    Easing easing = Easing::Linear;
    bool loop = false;  ///< true = 循环（autoReverse 来回摆动）
    /// 页面级播放控制（连续播放总开关）。为空 = 不参与连续播放。
    SharedPtr<AnimPlayback> playback;
    double canvasWidth = 360.0;
    double canvasHeight = 172.0;
};

class AnimDemo : public StatefulWidget {
public:
    explicit AnimDemo(AnimDemoSpec spec, KeyPtr key = nullptr)
        : StatefulWidget(std::move(key)), spec_(std::move(spec)) {}

    std::unique_ptr<State> createState() override;
    std::string toStringShort() const override { return "AnimDemo"; }

private:
    AnimDemoSpec spec_;
    friend class AnimDemoState;
};

class AnimDemoState : public State {
public:
    void initState() override {
        auto* w = static_cast<AnimDemo*>(widget());
        auto& spec = w->spec_;
        spec.clk->anim = cui::make_shared<AnimationController>(spec.durationMs);
        spec.clk->anim->setEasing(spec.easing);
        spec.clk->anim->setLoop(spec.loop);
        if (spec.loop) spec.clk->anim->setAutoReverse(true);
        spec.clk->run = [this, spec]() {
            spec.clk->anim->start();
            if (ticker_ && !ticker_->isActive()) ticker_->start();
        };
        spec.clk->halt = [this]() {
            if (ticker_) ticker_->stop();
        };
        spec.clk->resume = [this]() {
            if (ticker_ && !ticker_->isActive()) ticker_->start();
        };
        ticker_ = std::make_unique<Ticker>([this, spec](Duration e) {
            // 连续播放（页面级开关）：强制单向循环，播完自动从头再来。
            bool continuous = spec.playback && spec.playback->continuous;
            if (continuous) {
                auto* anim = spec.clk->anim.get();
                if (!anim->loop() || anim->autoReverse()) {
                    anim->setLoop(true);
                    anim->setAutoReverse(false);
                }
                if (!anim->isRunning())
                    anim->start();
            }
            spec.clk->anim->tick(e);  // 始终推进时钟并 emit progressChanged（驱动重绘）
            bool keep = spec.onTick ? spec.onTick(e) : spec.clk->anim->isRunning();
            if (continuous)
                keep = true;  // 连续播放：Ticker 保持运行
            if (!keep && ticker_) ticker_->stop();
        });

        // 页面级「连续播放」已经打开，但本 demo 是在开关打开之后才挂载的
        // （例如滚动进视口才构建）—— 此时必须立即开始播放，否则它会一直
        // 停在起点，直到用户点它的按钮。
        if (spec.playback && spec.playback->continuous) {
            spec.clk->anim->setLoop(true);
            spec.clk->anim->setAutoReverse(false);
            spec.clk->anim->start();
            ticker_->start();
        }
    }

    void dispose() override {
        if (ticker_) ticker_->stop();
    }

    SharedPtr<Widget> build(BuildContext& ctx) override {
        (void)ctx;
        auto& spec = static_cast<AnimDemo*>(widget())->spec_;
        auto anim = spec.clk->anim;
        auto paintFn = spec.paint;
        double ch = spec.canvasHeight;

        // 卡片水平占满：用 LayoutBuilder 取内容区可用宽度 availW，把画布宽度设为
        // availW 扣掉卡片外边距(12×2)与内容内边距(8×2)=40，于是卡片自然宽度恰等于
        // availW 而撑满整行；各 demo 的 paint 回调按 sz.width 自适应，动画随之铺开。
        // 不依赖 CrossAxisAlignment::Stretch（本框架 RenderFlex 未实现该语义，且会波及其它页）。
        return cui::make_shared<LayoutBuilder>(
            [this, anim, paintFn, ch](BuildContext&, const BoxConstraints& c) {
                double availW = c.maxWidth;
                if (!(availW > 0.0) || availW > 1e6)
                    availW = 400.0;  // 无界/异常回退（约等于原自然宽度）
                double canvasW = availW - 40.0;
                if (canvasW < 120.0)
                    canvasW = 120.0;
                return makeCard(canvasW, ch, anim, paintFn);
            });
    }

private:
    /// 组装单张 demo 卡片（标题 + 说明 + 动画画布 + 按钮），画布宽 cw 由外层
    /// LayoutBuilder 按可用宽度算出，使卡片水平占满内容区。
    SharedPtr<Widget> makeCard(
        double cw, double ch, const SharedPtr<AnimationController>& anim,
        const std::function<void(Painter&, const Size&, AnimationController&)>& paintFn) {
        auto& spec = static_cast<AnimDemo*>(widget())->spec_;
        auto builder = [paintFn, anim, cw, ch](BuildContext&, const SharedPtr<Animation<double>>&) {
            return cui::make_shared<CustomPaint>(
                [paintFn, anim](Painter& p, const Size& s) {
                    if (paintFn) paintFn(p, s, *anim);
                },
                nullptr, Size(cw, ch));
        };
        auto canvas = cui::make_shared<AnimatedBuilder>(anim, builder);

        std::vector<SharedPtr<Widget>> inner;
        inner.push_back(cui::make_shared<Text>(spec.title, TextStyle().size(14.0).bold()));
        inner.push_back(cui::make_shared<Text>(
            spec.caption, TextStyle().size(12.0).fgColor(theme().textSecondary)));
        inner.push_back(cui::make_shared<SizedBox>(0.0, 8.0));
        inner.push_back(canvas);
        inner.push_back(cui::make_shared<SizedBox>(0.0, 8.0));
        inner.push_back(buildButtons());

        auto col = cui::make_shared<Column>(std::move(inner));
        col->setMainAxisSize(MainAxisSize::Min);
        col->setCrossAxisAlignment(CrossAxisAlignment::Start);
        return cui::make_shared<Card>(col, EdgeInsets::all(12.0), theme().surface,
                                      10.0, 2.0);
    }

    SharedPtr<Widget> buildButtons() {
        auto* w = static_cast<AnimDemo*>(widget());
        auto& spec = w->spec_;
        std::vector<SharedPtr<Widget>> btns;
        for (const auto& b : spec.buttons) {
            btns.push_back(cui::make_shared<ElevatedButton>(cui::make_shared<Text>(b.first),
                                                            b.second));
            btns.push_back(cui::make_shared<SizedBox>(8.0, 0.0));
        }
        auto row = cui::make_shared<Row>(std::move(btns));
        row->setMainAxisSize(MainAxisSize::Min);
        return row;
    }

    std::unique_ptr<Ticker> ticker_;
};

inline std::unique_ptr<State> AnimDemo::createState() {
    return std::make_unique<AnimDemoState>();
}

/// 在画布上画一行基线辅助文本（基线对齐 y）。
inline void adDrawText(Painter& p, const std::string& s, double x, double y,
                       double size, const Color& c) {
    if (s.empty()) return;
    p.setFontSize(size);
    p.setFontBold(false);
    p.setSourceColor(c);
    p.save();
    p.translate(x, y - size);
    p.showText(s.c_str());
    p.restore();
}

}  // namespace cui
