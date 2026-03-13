/// @file ios_window.mm
/// @brief iOS/UIKit implementation of PlatformWindow.
///
/// 生命周期：宿主（CuiHost.mm）调 Application::createWindow → AppWindow 持有
/// 一个 IOSWindow。create() 在 UIKit 主线程建 UIWindow + CUIView（全屏）并
/// makeKeyAndVisible；渲染由 IOSWindow::presentPixels 把 Skia 的 CPU 像素拷到
/// CUIView.layer.contents（纯软件路径，与 OHOS 同构，无需 Metal/EGL 上下文）。
///
/// 帧泵在宿主侧用 CADisplayLink 驱动 Application::runIteration()，所以本文件的
/// waitForEvent 不阻塞（完全由 display link 节流，否则会冻住 UIKit 主 run loop）。
///
/// Retina：width()/height() 返回「设备像素」（逻辑点数 × UIScreen.scale），
/// Skia surface 据此按屏幕物理分辨率建；presentPixels 把整张设备像素位图交给
/// CALayer，由系统缩放到屏幕 → 文字/线条高清。触摸坐标在 forwardTouches 里
/// 同样 ×scale 换算到设备像素窗口空间（与 Android/OHOS 的设备像素窗口约定一致）。

#include <TargetConditionals.h>
#if defined(__APPLE__) && defined(__MACH__) && TARGET_OS_IPHONE

#import <UIKit/UIKit.h>
#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>  // CGImage / CGDataProvider / CGColorSpace

#include "cui/platform/ios_window.h"
#include "cui/platform/platform_window.h"

#include <mutex>

namespace {

// UIKit 的 UIKeyboardHIDUsage → cui_core::Key（只覆盖常用键，其余按字符上屏）。
int keyCodeFromUIKey(UIKey* key) {
    if (!key)
        return static_cast<int>(cui::KeyCode::None);
    switch (key.keyCode) {
        case UIKeyboardHIDUsageKeyboardReturnOrEnter:
        case UIKeyboardHIDUsageKeyboardInsertLine:
            return static_cast<int>(cui::KeyCode::Return);
        case UIKeyboardHIDUsageKeyboardDeleteOrBackspace:
            return static_cast<int>(cui::KeyCode::Backspace);
        case UIKeyboardHIDUsageKeyboardForwardDelete:
            return static_cast<int>(cui::KeyCode::Delete);
        case UIKeyboardHIDUsageKeyboardEscape:
            return static_cast<int>(cui::KeyCode::Escape);
        case UIKeyboardHIDUsageKeyboardTab:
            return static_cast<int>(cui::KeyCode::Tab);
        case UIKeyboardHIDUsageKeyboardHome:
            return static_cast<int>(cui::KeyCode::Home);
        case UIKeyboardHIDUsageKeyboardEnd:
            return static_cast<int>(cui::KeyCode::End);
        case UIKeyboardHIDUsageKeyboardPageUp:
            return static_cast<int>(cui::KeyCode::PageUp);
        case UIKeyboardHIDUsageKeyboardPageDown:
            return static_cast<int>(cui::KeyCode::PageDown);
        case UIKeyboardHIDUsageKeyboardUpArrow:
            return static_cast<int>(cui::KeyCode::Up);
        case UIKeyboardHIDUsageKeyboardDownArrow:
            return static_cast<int>(cui::KeyCode::Down);
        case UIKeyboardHIDUsageKeyboardLeftArrow:
            return static_cast<int>(cui::KeyCode::Left);
        case UIKeyboardHIDUsageKeyboardRightArrow:
            return static_cast<int>(cui::KeyCode::Right);
        default:
            return static_cast<int>(cui::KeyCode::None);
    }
}

}  // namespace

// ── CUIView ──────────────────────────────────────────────────────────────────

@interface CUIView : UIView
@property (nonatomic, assign) cui::IOSWindow* window;
@end

@implementation CUIView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.multipleTouchEnabled = YES;  // 多点触摸
        self.backgroundColor = [UIColor blackColor];
    }
    return self;
}

// 软件上屏：把 Skia 的 N32 像素拷进一份独立缓冲，再交给 CALayer.contents 显示。
- (void)presentPixels:(const void*)pixels rowBytes:(int)rowBytes width:(int)w height:(int)h {
    if (!pixels || rowBytes <= 0 || w <= 0 || h <= 0)
        return;
    const size_t len = (size_t)rowBytes * (size_t)h;
    void* copy = malloc(len);
    if (!copy)
        return;
    memcpy(copy, pixels, len);

    // Skia 在 iOS 上的 N32 是 BGRA（little-endian），与 CoreGraphics 的
    // kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little 对应。
    // 若实机出现红蓝互换，改这里字节序即可（无需动引擎其它部分）。
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        copy, copy, len,
        [](void* info, const void* data, size_t size) {
            (void)data; (void)size;
            free(info);
        });
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGImageRef img = CGImageCreate(
        w, h, 8, 32, rowBytes, cs,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little,
        provider, nullptr, false, kCGRenderingIntentDefault);
    // bridge_transfer：把 CGImage 的所有权交给 ARC 管理的 layer.contents。
    self.layer.contents = (__bridge_transfer id)img;
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(provider);
}

// ── 触摸 ──────────────────────────────────────────────────────────────────
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event;
    [self forwardTouches:touches phase:0];
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event;
    [self forwardTouches:touches phase:1];
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event;
    [self forwardTouches:touches phase:2];
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event;
    [self forwardTouches:touches phase:3];
}

- (void)forwardTouches:(NSSet<UITouch*>*)touches phase:(int)phase {
    if (!_window)
        return;
    // 触摸坐标来自 UIView（单位：点/points）。本窗口以「设备像素」上报尺寸
    // （width() = 点数 × scale），故这里把点换算成设备像素，与窗口空间一致。
    const double s = _window->contentScale();
    for (UITouch* t in touches) {
        const CGPoint p = [t locationInView:self];
        _window->onTouch((int)t.hash, p.x * s, p.y * s, phase);
    }
}

// ── 外接键盘（最小支持；软键盘/IME 暂未接入）────────────────────────────────
- (void)pressesBegan:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    (void)event;
    if (!_window)
        return;
    for (UIPress* press in presses) {
        if (!press.key)
            continue;
        UIKey* k = press.key;
        std::string text = k.characters ? std::string([k.characters UTF8String])
                                        : std::string();
        cui::PlatformEvent pe;
        pe.type = cui::EventType::KeyDown;
        pe.key = keyCodeFromUIKey(k);
        pe.text = text;
        pe.modifiers = 0;
        _window->pushEvent(pe);
    }
}

- (void)pressesEnded:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    (void)event;
    if (!_window)
        return;
    for (UIPress* press in presses) {
        if (!press.key)
            continue;
        UIKey* k = press.key;
        std::string text = k.characters ? std::string([k.characters UTF8String])
                                        : std::string();
        cui::PlatformEvent pe;
        pe.type = cui::EventType::KeyUp;
        pe.key = keyCodeFromUIKey(k);
        pe.text = text;
        pe.modifiers = 0;
        _window->pushEvent(pe);
    }
}

@end

// ── IOSWindow ────────────────────────────────────────────────────────────────

namespace cui {

IOSWindow::IOSWindow() = default;

IOSWindow::~IOSWindow() {
    destroy();
}

void IOSWindow::create(const std::string& title, int width, int height) {
    width_ = width;
    height_ = height;
    title_ = title;

    @autoreleasepool {
        // iOS 13+ 优先从 connectedScenes 取 windowScene；传统（无 Scene 的
        // Info.plist）模式下 connectedScenes 为空，退回 initWithFrame。
        UIWindowScene* scene = nil;
        for (UIScene* s in [UIApplication sharedApplication].connectedScenes) {
            if ([s isKindOfClass:[UIWindowScene class]]) {
                scene = (UIWindowScene*)s;
                break;
            }
        }
        UIWindow* window = scene
            ? [[UIWindow alloc] initWithWindowScene:scene]
            : [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];

        CUIView* view = [[CUIView alloc] initWithFrame:[UIScreen mainScreen].bounds];
        view.window = this;

        UIViewController* vc = [[UIViewController alloc] init];
        vc.view = view;

        window.rootViewController = vc;
        window.backgroundColor = [UIColor blackColor];
        [window makeKeyAndVisible];

        // UIScreen.scale = 设备像素/点（2/3 等）。它是 Retina 清晰度的关键：
        // width()/height() 与触摸坐标都按它放大到设备像素，Skia surface 随之
        // 按物理分辨率建，CALayer 再适配上屏。
        scale_ = static_cast<double>([UIScreen mainScreen].scale);

        uiWindow_ = (void*)CFBridgingRetain(window);
        uiView_ = (void*)CFBridgingRetain(view);
        viewController_ = (void*)CFBridgingRetain(vc);
    }

    PlatformEvent expose;
    expose.type = EventType::Expose;
    pushEvent(expose);
}

void IOSWindow::destroy() {
    if (uiWindow_) {
        CFBridgingRelease(uiWindow_);
        uiWindow_ = nullptr;
    }
    if (uiView_) {
        CFBridgingRelease(uiView_);
        uiView_ = nullptr;
    }
    if (viewController_) {
        CFBridgingRelease(viewController_);
        viewController_ = nullptr;
    }
}

void IOSWindow::show() {
    if (uiWindow_) {
        @autoreleasepool {
            [(__bridge UIWindow*)uiWindow_ makeKeyAndVisible];
        }
    }
}

void IOSWindow::invalidate() {
    // iOS 由 CADisplayLink 每帧重绘，无需在此请求；保持接口完整。
}

void IOSWindow::setTitle(const std::string& title) {
    title_ = title;  // iOS 全屏无标题栏
}

void IOSWindow::setMinimumSize(int width, int height) {
    (void)width;
    (void)height;  // iOS 全屏，无最小尺寸
}

double IOSWindow::dpiScale() const {
    return scale_ > 0.0 ? scale_ : 1.0;
}

double IOSWindow::contentScale() const {
    return scale_ > 0.0 ? scale_ : 1.0;
}

void IOSWindow::wakeUp() {
    // 帧泵由 CADisplayLink 驱动，无需唤醒。
}

bool IOSWindow::pollEvent(PlatformEvent& event) {
    std::lock_guard<std::mutex> lock(eventMutex_);
    if (pendingEvents_.empty())
        return false;
    event = pendingEvents_.front();
    pendingEvents_.pop();
    return true;
}

bool IOSWindow::waitForEvent(PlatformEvent& event, int timeoutMs) {
    // iOS 由 CADisplayLink 驱动 Application::runIteration()，这里绝不能阻塞
    // （否则会冻住 UIKit 主 run loop，触摸回调无法投递 → 死锁）。一律非阻塞。
    (void)timeoutMs;
    return pollEvent(event);
}

PlatformWindow::NativeHandles IOSWindow::nativeHandles() {
    NativeHandles h;
    // 借 ns_view 字段携带 UIView*，供将来 GPU 后端使用；当前呈现走 presentPixels。
    h.ns_view = uiView_;
    return h;
}

void IOSWindow::pushEvent(const PlatformEvent& event) {
    std::lock_guard<std::mutex> lock(eventMutex_);
    pendingEvents_.push(event);
}

void IOSWindow::resize(int width, int height) {
    width_ = width;
    height_ = height;
    PlatformEvent e;
    e.type = EventType::Resize;
    e.width = width;
    e.height = height;
    pushEvent(e);
}

void IOSWindow::onTouch(int touchId, double x, double y, int phase) {
    PlatformEvent pe;
    pe.pos = Point{x, y};
    pe.touchId = touchId;
    pe.button = 1;  // 触摸按左键上报，兼容只订了鼠标的手势
    switch (phase) {
        case 0:
            pe.type = EventType::TouchBegin;
            break;
        case 1:
            pe.type = EventType::TouchUpdate;
            break;
        case 2:
        case 3:  // cancel 按 end 处理
            pe.type = EventType::TouchEnd;
            break;
        default:
            return;
    }
    pushEvent(pe);

    // 与 Android / OHOS 支一致：触摸另配一条鼠标事件，鼠标手势才拿得到输入。
    PlatformEvent mouse = pe;
    mouse.type = (phase == 0) ? EventType::MouseDown
                : (phase == 1) ? EventType::MouseMove
                                : EventType::MouseUp;
    pushEvent(mouse);
}

bool IOSWindow::presentPixels(const void* pixels, int rowBytes, int x, int y,
                              int width, int height) {
    (void)x;
    (void)y;
    if (!uiView_)
        return false;
    UIView* view = (__bridge UIView*)uiView_;
    if (![view isKindOfClass:[CUIView class]])
        return false;
    [(CUIView*)view presentPixels:pixels rowBytes:rowBytes width:width height:height];
    return true;
}

}  // namespace cui_core

#endif  // __APPLE__ && __MACH__ && TARGET_OS_IPHONE
