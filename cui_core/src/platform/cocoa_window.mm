/// @file cocoa_window.mm
/// @brief Cocoa/AppKit implementation of PlatformWindow (macOS).

#include <TargetConditionals.h>

// AppKit is macOS-only: UIKit replaces it on iOS/tvOS/watchOS and Mac
// Catalyst, which are built through ios_window.mm instead.
#if defined(__APPLE__) && !TARGET_OS_IPHONE

#import "cui/platform/cocoa_window.h"

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

// ── Key code mapping ─────────────────────────────────────────────────────────

namespace {

// macOS virtual key codes (from HIToolbox/Events.h)
constexpr int kVK_Return = 0x24;
constexpr int kVK_Escape = 0x35;
constexpr int kVK_Delete = 0x33;  // Backspace
constexpr int kVK_Tab = 0x30;
constexpr int kVK_Space = 0x31;
constexpr int kVK_ForwardDelete = 0x75;
constexpr int kVK_Home = 0x73;
constexpr int kVK_End = 0x77;
constexpr int kVK_PageUp = 0x74;
constexpr int kVK_PageDown = 0x79;
constexpr int kVK_LeftArrow = 0x7B;
constexpr int kVK_RightArrow = 0x7C;
constexpr int kVK_DownArrow = 0x7D;
constexpr int kVK_UpArrow = 0x7E;

// Modifier flags
constexpr int kShiftKeyMask = 1 << 17;
constexpr int kControlKeyMask = 1 << 18;
constexpr int kOptionKeyMask = 1 << 19;  // Alt
constexpr int kCommandKeyMask = 1 << 20;  // Meta/Super

int keyFromMacKeyCode(int keyCode) {
    switch (keyCode) {
        case kVK_Return:      return static_cast<int>(cui::Key::Return);
        case kVK_Escape:      return static_cast<int>(cui::Key::Escape);
        case kVK_Delete:      return static_cast<int>(cui::Key::Backspace);
        case kVK_Tab:         return static_cast<int>(cui::Key::Tab);
        case kVK_ForwardDelete: return static_cast<int>(cui::Key::Delete);
        case kVK_Home:        return static_cast<int>(cui::Key::Home);
        case kVK_End:         return static_cast<int>(cui::Key::End);
        case kVK_PageUp:      return static_cast<int>(cui::Key::PageUp);
        case kVK_PageDown:    return static_cast<int>(cui::Key::PageDown);
        case kVK_LeftArrow:   return static_cast<int>(cui::Key::Left);
        case kVK_UpArrow:     return static_cast<int>(cui::Key::Up);
        case kVK_RightArrow:  return static_cast<int>(cui::Key::Right);
        case kVK_DownArrow:   return static_cast<int>(cui::Key::Down);
        default:              return static_cast<int>(cui::Key::None);
    }
}

int modifiersFromNSEvent(NSEvent* event) {
    NSEventModifierFlags flags = [event modifierFlags];
    int mods = 0;
    if (flags & NSEventModifierFlagShift)   mods |= cui::ModShift;
    if (flags & NSEventModifierFlagControl) mods |= cui::ModCtrl;
    if (flags & NSEventModifierFlagOption)  mods |= cui::ModAlt;
    if (flags & NSEventModifierFlagCommand) mods |= cui::ModMeta;
    return mods;
}

}  // namespace

// ── Custom NSView for event handling with NSTextInputClient ──────────────────

@interface CUIView : NSView <NSTextInputClient>

@property (nonatomic, assign) cui::CocoaWindow* window;
@property (nonatomic, strong) NSMutableAttributedString* markedText;
@property (nonatomic, assign) NSRange selectedRange;

@end

@implementation CUIView

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        _markedText = [[NSMutableAttributedString alloc] init];
        _selectedRange = NSMakeRange(0, 0);
    }
    return self;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (BOOL)isFlipped {
    return YES;  // Top-left origin (like Windows/X11)
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    // Add tracking area for mouse enter/leave and moved events
    if (_window) {
        NSTrackingArea* trackingArea = [[NSTrackingArea alloc]
            initWithRect:[self bounds]
                 options:NSTrackingMouseEnteredAndExited |
                         NSTrackingMouseMoved |
                         NSTrackingActiveInKeyWindow |
                         NSTrackingInVisibleRect
                   owner:self
                userInfo:nil];
        [self addTrackingArea:trackingArea];
    }
}

// ── Mouse events ─────────────────────────────────────────────────────────────

- (void)mouseDown:(NSEvent *)event {
    NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    int mods = modifiersFromNSEvent(event);
    _window->onMouseDown(pt.x, pt.y, 1, mods);
}

- (void)rightMouseDown:(NSEvent *)event {
    NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    int mods = modifiersFromNSEvent(event);
    _window->onMouseDown(pt.x, pt.y, 3, mods);
}

- (void)otherMouseDown:(NSEvent *)event {
    NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    int mods = modifiersFromNSEvent(event);
    int button = [event buttonNumber] + 1;  // 2=middle, 3+=extra
    _window->onMouseDown(pt.x, pt.y, button, mods);
}

- (void)mouseUp:(NSEvent *)event {
    NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    int mods = modifiersFromNSEvent(event);
    _window->onMouseUp(pt.x, pt.y, 1, mods);
}

- (void)rightMouseUp:(NSEvent *)event {
    NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    int mods = modifiersFromNSEvent(event);
    _window->onMouseUp(pt.x, pt.y, 3, mods);
}

- (void)otherMouseUp:(NSEvent *)event {
    NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    int mods = modifiersFromNSEvent(event);
    int button = [event buttonNumber] + 1;
    _window->onMouseUp(pt.x, pt.y, button, mods);
}

- (void)mouseMoved:(NSEvent *)event {
    NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    int mods = modifiersFromNSEvent(event);
    _window->onMouseMoved(pt.x, pt.y, mods);
}

- (void)mouseEntered:(NSEvent *)event {
    _window->onMouseEntered();
}

- (void)mouseExited:(NSEvent *)event {
    _window->onMouseExited();
}

- (void)scrollWheel:(NSEvent *)event {
    NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    int mods = modifiersFromNSEvent(event);
    _window->onScrollWheel([event scrollingDeltaX], [event scrollingDeltaY], pt.x, pt.y, mods);
}

// ── Keyboard events ──────────────────────────────────────────────────────────

- (void)keyDown:(NSEvent *)event {
    // Let NSTextInputClient handle the event for IME support
    [self interpretKeyEvents:@[event]];
}

- (void)keyUp:(NSEvent *)event {
    int keyCode = [event keyCode];
    int mods = modifiersFromNSEvent(event);
    _window->onKeyUp(keyCode, mods);
}

- (void)flagsChanged:(NSEvent *)event {
    int mods = modifiersFromNSEvent(event);
    _window->onFlagsChanged(mods);
}

// ── NSTextInputClient protocol ───────────────────────────────────────────────

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
    NSString* text = [string isKindOfClass:[NSAttributedString class]]
        ? [string string]
        : (NSString*)string;
    
    if (text.length > 0) {
        std::string utf8 = [text UTF8String];
        _window->onIMCommit(utf8);
    }
    
    // Clear marked text after commit
    [self unmarkText];
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {
    NSString* text = [string isKindOfClass:[NSAttributedString class]]
        ? [string string]
        : (NSString*)string;
    
    if (text.length > 0) {
        _markedText = [[NSMutableAttributedString alloc] initWithString:text];
        _selectedRange = selectedRange;
        
        std::string utf8 = [text UTF8String];
        _window->onIMPreedit(utf8);
    } else {
        [self unmarkText];
    }
}

- (void)unmarkText {
    _markedText = [[NSMutableAttributedString alloc] init];
    _selectedRange = NSMakeRange(0, 0);
    _window->onIMPreeditEnd();
}

- (NSRange)selectedRange {
    return _selectedRange;
}

- (NSRange)markedRange {
    return _markedText.length > 0 ? NSMakeRange(0, _markedText.length) : NSMakeRange(NSNotFound, 0);
}

- (BOOL)hasMarkedText {
    return _markedText.length > 0;
}

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    return nil;
}

- (NSArray*)validAttributesForMarkedText {
    return @[];
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    // Return the cursor position for IME candidate window
    NSWindow* window = [self window];
    NSRect frame = [window frame];
    NSRect viewFrame = [self frame];
    
    // Use the stored preedit position
    NSPoint pt = NSMakePoint(frame.origin.x + _window->imPreeditX(),
                             frame.origin.y + frame.size.height - viewFrame.size.height - _window->imPreeditY());
    
    NSRect rect = NSMakeRect(pt.x, pt.y, 0, 0);
    if (actualRange) *actualRange = range;
    return rect;
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    return 0;
}

- (void)doCommandBySelector:(SEL)selector {
    // Handle special keys that aren't text input
    if (selector == @selector(insertNewline:)) {
        int mods = modifiersFromNSEvent([NSApp currentEvent]);
        _window->onKeyDown(kVK_Return, "\n", mods);
    } else if (selector == @selector(insertTab:)) {
        int mods = modifiersFromNSEvent([NSApp currentEvent]);
        _window->onKeyDown(kVK_Tab, "\t", mods);
    } else if (selector == @selector(deleteBackward:)) {
        int mods = modifiersFromNSEvent([NSApp currentEvent]);
        _window->onKeyDown(kVK_Delete, "", mods);
    } else if (selector == @selector(deleteForward:)) {
        int mods = modifiersFromNSEvent([NSApp currentEvent]);
        _window->onKeyDown(kVK_ForwardDelete, "", mods);
    } else if (selector == @selector(moveLeft:)) {
        int mods = modifiersFromNSEvent([NSApp currentEvent]);
        _window->onKeyDown(kVK_LeftArrow, "", mods);
    } else if (selector == @selector(moveRight:)) {
        int mods = modifiersFromNSEvent([NSApp currentEvent]);
        _window->onKeyDown(kVK_RightArrow, "", mods);
    } else if (selector == @selector(moveUp:)) {
        int mods = modifiersFromNSEvent([NSApp currentEvent]);
        _window->onKeyDown(kVK_UpArrow, "", mods);
    } else if (selector == @selector(moveDown:)) {
        int mods = modifiersFromNSEvent([NSApp currentEvent]);
        _window->onKeyDown(kVK_DownArrow, "", mods);
    } else if (selector == @selector(moveToBeginningOfLine:)) {
        int mods = modifiersFromNSEvent([NSApp currentEvent]);
        _window->onKeyDown(kVK_Home, "", mods);
    } else if (selector == @selector(moveToEndOfLine:)) {
        int mods = modifiersFromNSEvent([NSApp currentEvent]);
        _window->onKeyDown(kVK_End, "", mods);
    } else if (selector == @selector(scrollPageUp:)) {
        int mods = modifiersFromNSEvent([NSApp currentEvent]);
        _window->onKeyDown(kVK_PageUp, "", mods);
    } else if (selector == @selector(scrollPageDown:)) {
        int mods = modifiersFromNSEvent([NSApp currentEvent]);
        _window->onKeyDown(kVK_PageDown, "", mods);
    } else if (selector == @selector(cancelOperation:)) {
        int mods = modifiersFromNSEvent([NSApp currentEvent]);
        _window->onKeyDown(kVK_Escape, "", mods);
    }
}

@end

// ── Custom NSWindowDelegate ──────────────────────────────────────────────────

@interface CUIDelegate : NSObject <NSWindowDelegate>

@property (nonatomic, assign) cui::CocoaWindow* window;

@end

@implementation CUIDelegate

- (void)windowWillClose:(NSNotification *)notification {
    cui::PlatformEvent e;
    e.type = cui::EventType::Close;
    _window->pushEvent(e);
}

- (void)windowDidResize:(NSNotification *)notification {
    NSWindow* win = [notification object];
    NSRect frame = [[win contentView] frame];
    _window->resize(frame.size.width, frame.size.height);
}

- (void)windowDidBecomeKey:(NSNotification *)notification {
    cui::PlatformEvent e;
    e.type = cui::EventType::FocusIn;
    _window->pushEvent(e);
}

- (void)windowDidResignKey:(NSNotification *)notification {
    cui::PlatformEvent e;
    e.type = cui::EventType::FocusOut;
    _window->pushEvent(e);
}

- (void)windowDidExpose:(NSNotification *)notification {
    cui::PlatformEvent e;
    e.type = cui::EventType::Expose;
    _window->pushEvent(e);
}

- (NSSize)windowWillResize:(NSWindow *)sender toSize:(NSSize)frameSize {
    if (_window) {
        NSSize minSize = NSMakeSize(_window->width(), _window->height());
        if (frameSize.width < minSize.width) frameSize.width = minSize.width;
        if (frameSize.height < minSize.height) frameSize.height = minSize.height;
    }
    return frameSize;
}

@end

// ── CocoaWindow implementation ───────────────────────────────────────────────

namespace cui {

CocoaWindow::CocoaWindow() {}

CocoaWindow::~CocoaWindow() {
    destroy();
}

void CocoaWindow::create(const std::string& title, int width, int height) {
    width_ = width;
    height_ = height;

    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        NSRect frame = NSMakeRect(0, 0, width, height);
        NSWindowStyleMask style = NSWindowStyleMaskTitled |
                                   NSWindowStyleMaskClosable |
                                   NSWindowStyleMaskMiniaturizable |
                                   NSWindowStyleMaskResizable;

        NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
                                                       styleMask:style
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        [window setTitle:[NSString stringWithUTF8String:title.c_str()]];
        [window center];

        CUIView* view = [[CUIView alloc] initWithFrame:frame];
        view.window = this;
        [window setContentView:view];

        CUIDelegate* delegate = [[CUIDelegate alloc] init];
        delegate.window = this;
        [window setDelegate:delegate];

        nsWindow_ = (__bridge void*)window;
        nsView_ = (__bridge void*)view;

        // Generate initial expose event (like X11)
        PlatformEvent exposeEvent;
        exposeEvent.type = EventType::Expose;
        pushEvent(exposeEvent);
    }
}

void CocoaWindow::destroy() {
    if (nsWindow_) {
        @autoreleasepool {
            NSWindow* window = (__bridge NSWindow*)nsWindow_;
            [window close];
        }
        nsWindow_ = nullptr;
        nsView_ = nullptr;
    }
}

void CocoaWindow::show() {
    if (nsWindow_) {
        @autoreleasepool {
            NSWindow* window = (__bridge NSWindow*)nsWindow_;
            [window makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
        }
    }
}

void CocoaWindow::invalidate() {
    if (nsView_) {
        @autoreleasepool {
            NSView* view = (__bridge NSView*)nsView_;
            [view setNeedsDisplay:YES];
        }
    }
}

void CocoaWindow::setTitle(const std::string& title) {
    if (nsWindow_) {
        @autoreleasepool {
            NSWindow* window = (__bridge NSWindow*)nsWindow_;
            [window setTitle:[NSString stringWithUTF8String:title.c_str()]];
        }
    }
}

void CocoaWindow::setMinimumSize(int width, int height) {
    minWidth_ = width;
    minHeight_ = height;
    if (nsWindow_) {
        @autoreleasepool {
            NSWindow* window = (__bridge NSWindow*)nsWindow_;
            [window setContentMinSize:NSMakeSize(width, height)];
        }
    }
}

double CocoaWindow::dpiScale() const {
    if (nsWindow_) {
        @autoreleasepool {
            NSWindow* window = (__bridge NSWindow*)nsWindow_;
            return [window backingScaleFactor];
        }
    }
    return 1.0;
}

void CocoaWindow::wakeUp() {
    @autoreleasepool {
        NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                            location:NSZeroPoint
                                       modifierFlags:0
                                           timestamp:0
                                        windowNumber:0
                                             context:nil
                                             subtype:0
                                               data1:0
                                               data2:0];
        [NSApp postEvent:event atStart:YES];
    }
}

void CocoaWindow::grabPointer() {
    // macOS doesn't have a direct pointer grab API like X11/Win32
    // We can use CGAssociateMouseAndMouseCursorPosition to hide cursor
    // For now, just mark as grabbed
    pointerGrabbed_ = true;
}

void CocoaWindow::ungrabPointer() {
    pointerGrabbed_ = false;
}

bool CocoaWindow::clipboardSetText(const std::string& utf8) {
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        NSString* str = [NSString stringWithUTF8String:utf8.c_str()];
        return [pb setString:str forType:NSPasteboardTypeString] == YES;
    }
}

std::string CocoaWindow::clipboardText() const {
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        NSString* str = [pb stringForType:NSPasteboardTypeString];
        if (str) {
            return std::string([str UTF8String]);
        }
    }
    return {};
}

bool CocoaWindow::hasClipboardText() const {
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        return [pb availableTypeFromArray:@[NSPasteboardTypeString]] != nil;
    }
}

// ── IME ──────────────────────────────────────────────────────────────────────

bool CocoaWindow::takeIMCommit(std::string& utf8) {
    if (imCommits_.empty())
        return false;
    utf8 = std::move(imCommits_.front());
    imCommits_.pop_front();
    return true;
}

std::string CocoaWindow::imPreedit() const {
    return imPreedit_;
}

void CocoaWindow::setIMPreeditRect(int x, int y) {
    imPreeditX_ = x;
    imPreeditY_ = y;
}

void CocoaWindow::onIMCommit(const std::string& text) {
    if (!text.empty()) {
        imCommits_.push_back(text);
    }
}

void CocoaWindow::onIMPreedit(const std::string& text) {
    imPreedit_ = text;
}

void CocoaWindow::onIMPreeditEnd() {
    imPreedit_.clear();
}

// ── Event handling ───────────────────────────────────────────────────────────

bool CocoaWindow::pollEvent(PlatformEvent& event) {
    @autoreleasepool {
        NSEvent* nsEvent = [NSApp nextEventMatchingMask:NSEventMaskAny
                                              untilDate:[NSDate distantPast]
                                                 inMode:NSDefaultRunLoopMode
                                                dequeue:YES];
        if (nsEvent) {
            [NSApp sendEvent:nsEvent];
        }
    }
    if (!pendingEvents_.empty()) {
        event = pendingEvents_.front();
        pendingEvents_.pop();
        return true;
    }
    return false;
}

bool CocoaWindow::waitForEvent(PlatformEvent& event, int timeoutMs) {
    if (timeoutMs == 0) return pollEvent(event);

    @autoreleasepool {
        NSDate* date = (timeoutMs < 0)
            ? [NSDate distantFuture]
            : [NSDate dateWithTimeIntervalSinceNow:timeoutMs / 1000.0];

        NSEvent* nsEvent = [NSApp nextEventMatchingMask:NSEventMaskAny
                                              untilDate:date
                                                 inMode:NSDefaultRunLoopMode
                                                dequeue:YES];
        if (nsEvent) {
            [NSApp sendEvent:nsEvent];
        }
    }

    if (!pendingEvents_.empty()) {
        event = pendingEvents_.front();
        pendingEvents_.pop();
        return true;
    }
    return false;
}

PlatformWindow::NativeHandles CocoaWindow::nativeHandles() {
    NativeHandles h;
    h.ns_window = nsWindow_;
    h.ns_view = nsView_;
    return h;
}

void CocoaWindow::pushEvent(const PlatformEvent& event) {
    pendingEvents_.push(event);
}

void CocoaWindow::resize(int width, int height) {
    width_ = width;
    height_ = height;

    PlatformEvent e;
    e.type = EventType::Resize;
    e.width = width;
    e.height = height;
    pushEvent(e);
}

// ─ Event handlers ───────────────────────────────────────────────────────────

void CocoaWindow::onMouseDown(double x, double y, int button, int modifiers) {
    PlatformEvent e;
    e.type = EventType::MouseDown;
    e.pos = {x, y};
    e.button = button;
    e.modifiers = modifiers;
    pushEvent(e);
}

void CocoaWindow::onMouseUp(double x, double y, int button, int modifiers) {
    PlatformEvent e;
    e.type = EventType::MouseUp;
    e.pos = {x, y};
    e.button = button;
    e.modifiers = modifiers;
    pushEvent(e);
}

void CocoaWindow::onMouseMoved(double x, double y, int modifiers) {
    PlatformEvent e;
    e.type = EventType::MouseMove;
    e.pos = {x, y};
    e.modifiers = modifiers;
    pushEvent(e);
}

void CocoaWindow::onMouseEntered() {
    // Mouse entered the view - could generate a hover event if needed
}

void CocoaWindow::onMouseExited() {
    // Mouse exited the view - could generate a hover event if needed
}

void CocoaWindow::onScrollWheel(double deltaX, double deltaY, double x, double y, int modifiers) {
    PlatformEvent e;
    e.type = EventType::MouseWheel;
    e.pos = {x, y};
    e.deltaX = deltaX;
    e.deltaY = deltaY;
    e.modifiers = modifiers;
    pushEvent(e);
}

void CocoaWindow::onKeyDown(int keyCode, const std::string& characters, int modifiers) {
    PlatformEvent e;
    e.type = EventType::KeyDown;
    e.key = keyFromMacKeyCode(keyCode);
    e.text = characters;
    e.modifiers = modifiers;
    pushEvent(e);
}

void CocoaWindow::onKeyUp(int keyCode, int modifiers) {
    PlatformEvent e;
    e.type = EventType::KeyUp;
    e.key = keyFromMacKeyCode(keyCode);
    e.modifiers = modifiers;
    pushEvent(e);
}

void CocoaWindow::onFlagsChanged(int modifiers) {
    currentModifiers_ = modifiers;
}

}  // namespace cui_core

#endif  // __APPLE__ && !TARGET_OS_IPHONE
