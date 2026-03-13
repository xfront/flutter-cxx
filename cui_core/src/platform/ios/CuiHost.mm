/// @file CuiHost.mm
/// @brief iOS 宿主入口（ObjC++，对应 android/ohos 的 cpp/main.cpp）。
///
/// 与 android/ohos 同构：建 Application → beginEventLoop → createWindow →
/// setRoot(cui_core::createAppRoot()) → renderFrame，然后进入帧循环。但 iOS 不用
/// 自带 while 循环（会冻 UIKit 主 run loop），而是用 CADisplayLink 在 run loop
/// 上每帧调 Application::runIteration()。
///
/// 入口：Swift 侧 CuiApp（@UIApplicationMain）在
/// application(_:didFinishLaunchingWithOptions:) 里调 cuiHostBoot()，退出前
/// applicationWillTerminate 里调 cuiHostShutdown()。

#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>

#include <cstdlib>

#include "cui/foundation/application.h"
#include "cui/foundation/translator.h"
#include "cui/foundation/window.h"
#include "cui/framework/app_root.h"  // cui_core::createAppRoot()：由应用层实现的注入点
#include "cui/platform/ios_window.h"

namespace {
cui::Application* g_app = nullptr;
CuiHost* g_host = nullptr;       // Objective-C 对象，持有 display link selector
CADisplayLink* g_link = nullptr;
}  // namespace

// ── CuiHost（ObjC 对象，承载 CADisplayLink 的 selector）──────────────────────

@interface CuiHost : NSObject
- (void)tick:(CADisplayLink*)link;
@end

@implementation CuiHost

- (void)tick:(CADisplayLink*)link {
    if (!g_app)
        return;
    // runIteration：drain 事件 → 平台消息 → ticker → 任务（含渲染 + 上屏）。
    // IOSWindow::waitForEvent 非阻塞，所以这里不会冻住 run loop。
    if (!g_app->runIteration()) {
        [link invalidate];
        g_link = nullptr;
    }
}

@end

// ── C 入口（Swift 通过 bridging header 调用）────────────────────────────────

extern "C" void cuiHostBoot() {
    if (g_app)
        return;  // 只起一次

    CGRect bounds = [UIScreen mainScreen].bounds;
    const int w = static_cast<int>(round(bounds.size.width));
    const int h = static_cast<int>(round(bounds.size.height));

    g_app = new cui::Application();

    // 本线程（didFinishLaunching）就是 GUI 线程：beginEventLoop 置 running_、
    // 定框线程 id、接好平台通道的 wakeUp 钩子。不调的话 runIteration 第一拍
    // 返回 false，循环空转一次就退。
    g_app->beginEventLoop();

    if (!g_app->createWindow("CUI", w, h)) {
        // 屏幕几何未就绪：这里屏幕尺寸来自 UIScreen，理论不会失败。
        return;
    }

    cui::Translator::instance().setLanguage("zh");
    g_app->setRoot(cui::createAppRoot());
    g_app->renderFrame(/*forceFull=*/true);

    // CADisplayLink 在主线 run loop 上驱动帧；~60fps 由 display link 节流。
    g_host = [[CuiHost alloc] init];
    g_link = [CADisplayLink displayLinkWithTarget:g_host
                                         selector:@selector(tick:)];
    [g_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

extern "C" void cuiHostShutdown() {
    if (g_link) {
        [g_link invalidate];
        g_link = nullptr;
    }
    g_host = nullptr;
    if (g_app) {
        g_app->quit();
        g_app->endEventLoop();
        delete g_app;
        g_app = nullptr;
    }
}
