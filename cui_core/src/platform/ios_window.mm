/// @file ios_window.mm
/// @brief iOS/UIKit implementation of PlatformWindow.

#include <TargetConditionals.h>

#ifdef __APPLE__
#if TARGET_OS_IOS || TARGET_OS_IPHONE

#import "cui/platform/ios_window.h"

#import <UIKit/UIKit.h>

// ── Custom UIView for event handling ─────────────────────────────────────────

@interface CUIView : UIView

@property (nonatomic, assign) cui::IOSWindow* window;

@end

@implementation CUIView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.multipleTouchEnabled = YES;
    }
    return self;
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for (UITouch* touch in touches) {
        CGPoint point = [touch locationInView:self];
        int touchId = (int)[touch hash];
        _window->onTouch(touchId, point.x, point.y, 0);  // 0 = began
    }
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for (UITouch* touch in touches) {
        CGPoint point = [touch locationInView:self];
        int touchId = (int)[touch hash];
        _window->onTouch(touchId, point.x, point.y, 1);  // 1 = moved
    }
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for (UITouch* touch in touches) {
        CGPoint point = [touch locationInView:self];
        int touchId = (int)[touch hash];
        _window->onTouch(touchId, point.x, point.y, 2);  // 2 = ended
    }
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for (UITouch* touch in touches) {
        CGPoint point = [touch locationInView:self];
        int touchId = (int)[touch hash];
        _window->onTouch(touchId, point.x, point.y, 3);  // 3 = cancelled
    }
}

- (void)layoutSubviews {
    [super layoutSubviews];
    CGRect bounds = self.bounds;
    _window->resize(bounds.size.width, bounds.size.height);
}

@end

// ── UIViewController ─────────────────────────────────────────────────────────

@interface CUIViewController : UIViewController

@property (nonatomic, assign) cui::IOSWindow* window;

@end

@implementation CUIViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor whiteColor];

    CUIView* view = [[CUIView alloc] initWithFrame:self.view.bounds];
    view.window = _window;
    view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:view];
}

- (BOOL)prefersStatusBarHidden {
    return NO;
}

@end

// ── IOSWindow implementation ─────────────────────────────────────────────────

namespace cui {

IOSWindow::IOSWindow() {}

IOSWindow::~IOSWindow() {
    destroy();
}

void IOSWindow::create(const std::string& title, int width, int height) {
    width_ = width;
    height_ = height;
    title_ = title;

    @autoreleasepool {
        // Create UIWindow
        UIWindow* window = [[UIWindow alloc] initWithFrame:CGRectMake(0, 0, width, height)];
        window.backgroundColor = [UIColor whiteColor];

        // Create UIViewController
        CUIViewController* vc = [[CUIViewController alloc] init];
        vc.window = this;
        window.rootViewController = vc;

        // Get scale factor
        scale_ = window.screen.scale;

        uiWindow_ = (__bridge void*)window;
        viewController_ = (__bridge void*)vc;

        // Get the view
        uiView_ = (__bridge void*)vc.view;
    }
}

void IOSWindow::destroy() {
    if (uiWindow_) {
        @autoreleasepool {
            UIWindow* window = (__bridge UIWindow*)uiWindow_;
            window.hidden = YES;
        }
        uiWindow_ = nullptr;
        uiView_ = nullptr;
        viewController_ = nullptr;
    }
}

void IOSWindow::show() {
    if (uiWindow_) {
        @autoreleasepool {
            UIWindow* window = (__bridge UIWindow*)uiWindow_;
            [window makeKeyAndVisible];
        }
    }
}

void IOSWindow::invalidate() {
    if (uiView_) {
        @autoreleasepool {
            UIView* view = (__bridge UIView*)uiView_;
            [view setNeedsDisplay];
        }
    }
}

void IOSWindow::setTitle(const std::string& title) {
    title_ = title;
    // iOS doesn't have a window title in the traditional sense
}

void IOSWindow::setMinimumSize(int width, int height) {
    // iOS manages sizing via Auto Layout
}

double IOSWindow::dpiScale() const {
    return scale_;
}

bool IOSWindow::pollEvent(PlatformEvent& event) {
    std::lock_guard<std::mutex> lock(eventMutex_);
    if (!pendingEvents_.empty()) {
        event = pendingEvents_.front();
        pendingEvents_.pop();
        return true;
    }
    return false;
}

bool IOSWindow::waitForEvent(PlatformEvent& event, int timeoutMs) {
    if (timeoutMs == 0) return pollEvent(event);

    // iOS uses RunLoop for event processing
    @autoreleasepool {
        NSDate* date = (timeoutMs < 0)
            ? [NSDate distantFuture]
            : [NSDate dateWithTimeIntervalSinceNow:timeoutMs / 1000.0];

        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:date];
    }

    return pollEvent(event);
}

void IOSWindow::wakeUp() {
    @autoreleasepool {
        CFRunLoopWakeUp(CFRunLoopGetCurrent());
    }
}

PlatformWindow::NativeHandles IOSWindow::nativeHandles() {
    NativeHandles h;
    h.ns_window = uiWindow_;  // Reuse for UIWindow*
    h.ns_view = uiView_;      // Reuse for UIView*
    return h;
}

void IOSWindow::pushEvent(const PlatformEvent& event) {
    std::lock_guard<std::mutex> lock(eventMutex_);
    pendingEvents_.push(event);
}

void IOSWindow::resize(int width, int height) {
    if (width != width_ || height != height_) {
        width_ = width;
        height_ = height;

        PlatformEvent event;
        event.type = EventType::Resize;
        event.width = width;
        event.height = height;
        pushEvent(event);
    }
}

void IOSWindow::onTouch(int touchId, double x, double y, int phase) {
    PlatformEvent event;
    event.pos = {x, y};
    event.touchId = touchId;

    switch (phase) {
    case 0: event.type = EventType::TouchBegin; break;
    case 1: event.type = EventType::TouchUpdate; break;
    case 2: event.type = EventType::TouchEnd; break;
    case 3: event.type = EventType::TouchEnd; break;  // Cancelled
    }

    pushEvent(event);
}

} // namespace cui_core

#endif // TARGET_OS_IOS
#endif // __APPLE__
