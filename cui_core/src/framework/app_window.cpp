/// @file framework/app_window.cpp
/// @brief AppWindow — per-top-level-window platform surface + render pipeline.

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <include/core/SkBlendMode.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkSurface.h>
#include <include/encode/SkPngEncoder.h>
#include <thread>
#include <vector>

#include "cui/framework/build_owner.h"
#include "cui/framework/framework.h"
#include "cui/foundation/application.h"
#include "cui/foundation/theme.h"
#include "cui/foundation/window.h"
#include "cui/platform/platform_factory.h"
#include "cui/reactive/scheduler_binding.h"
#include "cui/rendering/render_object.h"
#include "cui/rendering/render_view.h"
#include "cui/rendering/skia_renderer.h"

#if defined(__EMSCRIPTEN__)
// presentPixelsToCanvas() 用 EM_ASM()；这个宏住在 emscripten/emscripten.h，
// 少了它 EM_ASM 不展开，JS 体会被当成 C++ 解析（$0 报 expected expression）。
#include <emscripten/emscripten.h>
#elif defined(__ANDROID__)
// ANativeWindow 呈现（blitToNativeWindow 的 Android 分支）。
// 日志必须走 __android_log_print：应用进程的 stdout/stderr 不会进 logcat。
#include <android/log.h>
#include <android/native_window.h>
#elif defined(__linux__) && !defined(__OHOS__)
// 鸿蒙的 triple 带 -linux- ⇒ __linux__ 也为 1，但 NDK sysroot 没有 xcb。
#include <xcb/xcb.h>
#elif defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
// iOS 呈现分支需要 TARGET_OS_IPHONE（来自 SDK 的 TargetConditionals.h）。
#include <TargetConditionals.h>
#endif

namespace cui {

AppWindow::AppWindow(Application& app, std::string title, int width, int height)
    : app_(app),
      title_(std::move(title)),
      winWidth_(width),
      winHeight_(height),
      renderer_(std::make_unique<SkiaRenderer>()),
      schedulerBinding_(std::make_unique<SchedulerBinding>()),
      pipelineOwner_(std::make_unique<PipelineOwner>()),
      renderView_(std::make_unique<RenderView>()) {
    schedulerBinding_->setPipelineOwner(pipelineOwner_.get());
    schedulerBinding_->setRenderView(renderView_.get());
    renderView_->setWindowSize({static_cast<double>(winWidth_), static_cast<double>(winHeight_)});
}

AppWindow::~AppWindow() {
    // See window.h: the element tree must be unmounted while this window's
    // PipelineOwner/RenderView are still alive.
    buildOwner_.reset();
}

// ── creation ────────────────────────────────────────────────────────────────

bool AppWindow::createNative(bool borderless) {
#if defined(__linux__) && !defined(__ANDROID__)
    // Xlib's XIM/Xutf8* paths pick their encoding from LC_CTYPE; without
    // this the C locale makes Xutf8LookupString mangle multi-byte commits.
    std::setlocale(LC_CTYPE, "");
#endif

    platformWindow_ = createPlatformWindow();
    if (!platformWindow_)
        return false;

    platformWindow_->create(title_, winWidth_, winHeight_);
    borderless_ = borderless;
    if (borderless)
        platformWindow_->setBorderless(true);

    // The backend may derive its size from the host (web: CSS box × DPR);
    // keep the Skia surface in step with what the window actually reports.
    winWidth_ = platformWindow_->width();
    winHeight_ = platformWindow_->height();
    recreateSkiaSurface(winWidth_, winHeight_);
    platformWindow_->show();
    open_ = true;
    return true;
}

bool AppWindow::createOffscreen() {
    recreateSkiaSurface(winWidth_, winHeight_);
    open_ = true;
    return true;
}

void AppWindow::destroyNative() {
    if (platformWindow_) {
        platformWindow_->destroy();
        platformWindow_.reset();
    }
}

// ── widget tree ─────────────────────────────────────────────────────────────

void AppWindow::setRoot(SharedPtr<Widget> root) {
    // Detach any previous widget root from this window's pipeline.
    if (rootEl_ && renderView_ && pipelineOwner_) {
        if (auto* oldBox = dynamic_cast<RenderBox*>(rootEl_->findRenderObject()))
            pipelineOwner_->clearRootNode(oldBox);
    }

    if (!buildOwner_)
        buildOwner_ = std::make_unique<BuildOwner>();

    // 上游 WidgetsBinding._handleBuildScheduled：脏列表一出现待办就
    // ensureVisualUpdate → scheduleFrame。缺了这一步，setState 只标记脏、
    // 不请求帧，界面要等下一次输入事件才刷新。
    buildOwner_->onBuildScheduled = [this] { app_.scheduleRender(); };

    auto element = root->createElement(root);
    rootEl_ = element.get();
    buildOwner_->setRootRenderObjectHost(renderView_.get());
    buildOwner_->adoptRootElement(std::move(element));
    buildOwner_->flushBuildScope();
    schedulerBinding_->setBuildOwner(buildOwner_.get());

    if (auto* box = dynamic_cast<RenderBox*>(rootEl_->findRenderObject())) {
        renderView_->setChild(box);
        pipelineOwner_->setRootNode(renderView_.get());
    }
    hasRoot_ = true;
}

// ── native window controls ──────────────────────────────────────────────────

void AppWindow::setTitle(const std::string& title) {
    title_ = title;
    if (platformWindow_)
        platformWindow_->setTitle(title);
}

void AppWindow::setMinimumSize(int width, int height) {
    if (platformWindow_)
        platformWindow_->setMinimumSize(width, height);
}

void AppWindow::setBorderless(bool borderless) {
    borderless_ = borderless;
    if (platformWindow_)
        platformWindow_->setBorderless(borderless);
}

void AppWindow::show() {
    if (platformWindow_)
        platformWindow_->show();
}

int AppWindow::width() const {
    return platformWindow_ ? platformWindow_->width() : winWidth_;
}

int AppWindow::height() const {
    return platformWindow_ ? platformWindow_->height() : winHeight_;
}

void AppWindow::close() {
    if (!open_)
        return;
    open_ = false;
    destroyNative();
}

// ── rendering ───────────────────────────────────────────────────────────────

void AppWindow::recreateSkiaSurface(int width, int height) {
    if (!renderer_)
        return;

#if defined(__linux__) && !defined(__ANDROID__) && CUI_SKIA_GRAPHITE_VULKAN
    // Try swapchain-based rendering for true GPU acceleration
    if (platformWindow_) {
        auto handles = platformWindow_->nativeHandles();
        if (handles.xcb_connection && handles.xcb_window) {
            if (renderer_->initWithXcbWindow(
                    static_cast<xcb_connection_t*>(handles.xcb_connection),
                    static_cast<xcb_window_t>(handles.xcb_window),
                    width, height)) {
                return;
            }
        }
    }
#endif
    // Fall back to regular init (raster or GPU without swapchain)
    renderer_->init(width, height);
}

void AppWindow::applyResize(int width, int height) {
    if (width == winWidth_ && height == winHeight_)
        return;
    winWidth_ = width;
    winHeight_ = height;
    recreateSkiaSurface(winWidth_, winHeight_);
    if (renderView_)
        renderView_->setWindowSize({(double)winWidth_, (double)winHeight_});
    // Layout happens in renderFrame() (flushLayout cascade).
    renderFrame(/*forceFull=*/true);
}

void AppWindow::renderFrame(bool forceFull) {
    if (!hasRoot_)
        return;
    if (!renderer_ || !renderer_->canvas())
        return;
    if (rendering_) {
        // A signal handler or widget callback triggered a frame while one is
        // already being rendered: defer to the next scheduled frame.
        scheduleRender();
        return;
    }
    rendering_ = true;

    // Use the platform window's actual size — winWidth_/winHeight_ may be
    // stale if renderFrame() is called between Resize events.
    int drawW = winWidth_;
    int drawH = winHeight_;
    if (platformWindow_) {
        drawW = platformWindow_->width();
        drawH = platformWindow_->height();
    }

    // Grow the Skia surface when the window was resized without a Resize
    // event being dispatched yet; otherwise the frame is painted onto a stale
    // smaller surface and the blit is clipped/corrupted.
    if (drawW != winWidth_ || drawH != winHeight_) {
        winWidth_ = drawW;
        winHeight_ = drawH;
        recreateSkiaSurface(winWidth_, winHeight_);
    }

    paintFullFrame(drawW, drawH, forceFull);

    // 帧末回调（上游 SchedulerPhase.postFrameCallbacks）。放在
    // paintFullFrame 之后、且在「本帧无视觉变化」那条提前 return 之外。
    if (buildOwner_)
        buildOwner_->flushPostFrameCallbacks();

    rendering_ = false;
}

void AppWindow::scheduleRender() {
    app_.scheduleRender();
}

void AppWindow::paintFullFrame(int drawW, int drawH, bool forceFull) {
    SkCanvas* canvas = renderer_->canvas();
    const auto& bg = theme().background;
    const SkColor bgSk = SkColorSetRGB(static_cast<uint8_t>(bg.r * 255),
                                       static_cast<uint8_t>(bg.g * 255),
                                       static_cast<uint8_t>(bg.b * 255));

    // 先只跑管线（flush build/layout/compositing/paint，把脏边界录制进
    // LayerTree）得到本帧脏区；再据脏区决定 clear / composite（clip 到脏区）
    // / blit 范围。
    const bool havePipeline = renderView_ && pipelineOwner_ && schedulerBinding_;
    SchedulerBinding::FrameDamage dmg;
    if (havePipeline) {
        renderView_->setWindowSize({(double)drawW, (double)drawH});
        dmg = schedulerBinding_->runPipelineAndComputeDamage();
    } else {
        dmg.all = true;  // 无管线：保守全清全屏
    }
    if (forceFull)
        dmg.all = true;

    // 无视觉变化：本帧没有任何边界被重录，LayerTree 与 surface 均未变 →
    // 跳过整帧 clear/composite/blit。
    if (!dmg.any())
        return;

    presentDamageAll_ = dmg.all;
    presentDamage_ = dmg.rect;

    if (!dmg.all) {
        const SkRect dr = SkRect::MakeLTRB(static_cast<SkScalar>(dmg.rect.x),
                                           static_cast<SkScalar>(dmg.rect.y),
                                           static_cast<SkScalar>(dmg.rect.x + dmg.rect.width),
                                           static_cast<SkScalar>(dmg.rect.y + dmg.rect.height));
        canvas->save();
        canvas->clipRect(dr);
        // SkCanvas::clear 忽略裁剪，故用 Src 混合的 drawRect 把脏区刷成背景。
        SkPaint clearPaint;
        clearPaint.setBlendMode(SkBlendMode::kSrc);
        clearPaint.setColor(bgSk);
        canvas->drawRect(dr, clearPaint);
        if (havePipeline)
            schedulerBinding_->compositeTo(canvas);
        canvas->restore();
    } else {
        canvas->clear(bgSk);
        if (havePipeline)
            schedulerBinding_->compositeTo(canvas);
    }

    blitToNativeWindow();
}

bool AppWindow::saveScreenshot(const std::string& pngPath) {
    if (!renderer_ || !renderer_->canvas())
        return false;

    const int w = renderer_->width();
    const int h = renderer_->height();
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    if (!renderer_->readPixels(pixels.data(), w, h)) {
        std::fprintf(stderr, "cui_core: screenshot: readPixels failed (%dx%d)\n", w, h);
        return false;
    }

    SkImageInfo info = SkImageInfo::MakeN32Premul(w, h);
    // MakeWithCopy（而非 MakeFromMalloc）：MakeFromMalloc 会接管
    // pixels.data() 的所有权并在析构时 free()，与 vector 的析构构成
    // double free。
    auto image = SkImages::RasterFromData(info, SkData::MakeWithCopy(pixels.data(), pixels.size()), w * 4);
    if (!image)
        return false;

    auto png = SkPngEncoder::Encode(nullptr, image.get(), SkPngEncoder::Options{});
    if (!png)
        return false;

    FILE* f = std::fopen(pngPath.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(png->data(), 1, png->size(), f) == png->size();
    std::fclose(f);
    if (ok)
        std::fprintf(stderr, "cui_core: screenshot saved to %s (%zu bytes)\n", pngPath.c_str(), png->size());
    return ok;
}

#if defined(__EMSCRIPTEN__)
namespace {

/// Copy a BGRx/N32 Skia pixel block into the page canvas (single-window web
/// backend; additional offscreen AppWindows never reach this path).
void presentPixelsToCanvas(const uint8_t* pixels, int stride, int x, int y, int w, int h) {
    if (!pixels || w <= 0 || h <= 0)
        return;
    // The JS body has to be wrapped in parentheses: the preprocessor splits
    // macro arguments on top-level commas, and braces alone do not protect
    // them (em_asm.h: "If the code block ... contains a comma, then wrap the
    // whole code block inside parentheses").
    EM_ASM(({
        const ptr = $0, stride = $1, x = $2, y = $3, w = $4, h = $5;
        const canvas = Module['canvas'] || document.querySelector('#canvas');
        if (!canvas) return;
        let ctx = Module['_cuiCanvasCtx'];
        if (!ctx) {
            ctx = canvas.getContext('2d', { alpha: false });
            Module['_cuiCanvasCtx'] = ctx;
        }
        if (!ctx) return;
        let image = Module['_cuiImageData'];
        if (!image || image.width !== w || image.height !== h) {
            image = ctx.createImageData(w, h);
            Module['_cuiImageData'] = image;
        }
        const dst = new Uint32Array(image.data.buffer);
        const heap = HEAPU8.buffer;
        for (let row = 0; row < h; ++row) {
            const src = new Uint32Array(heap, ptr + row * stride, w);
            const base = row * w;
            for (let i = 0; i < w; ++i) {
                const p = src[i];
                dst[base + i] = 0xff000000 | ((p & 0xff) << 16) | (p & 0xff00) |
                                ((p >>> 16) & 0xff);
            }
        }
        ctx.putImageData(image, x, y);
    }), pixels, stride, x, y, w, h);
}

}  // namespace
#endif

void AppWindow::blitToNativeWindow() {
    if (!renderer_)
        return;

    // dev-1 验证过的呈现架构：GPU(Graphite) 渲染 → readPixels 读回 CPU →
    // xcb_put_image 上屏。
#if !defined(__EMSCRIPTEN__)
    if (!platformWindow_)
        return;  // offscreen window: nothing to present
    auto handles = platformWindow_->nativeHandles();
#endif

#if defined(__ANDROID__)
    // Android 的 Present：ANativeWindow 双缓冲 → 只能整帧提交（部分脏区
    // 落到未初始化的后备缓冲会花屏），所以这里忽略 presentDamage_ 恒全量
    // 拷贝。像素格式 RGBA_8888 与 Skia 的 N32 一致，按行 memcpy 即可。
    auto* nativeWindow = static_cast<ANativeWindow*>(handles.android_window);
    if (!nativeWindow)
        return;  // surface 尚未就绪（Activity 还没 onNativeWindowCreated）

    const int blitW = platformWindow_->width();
    const int blitH = platformWindow_->height();
    if (blitW <= 0 || blitH <= 0)
        return;

    // 下面的拷贝假定 4 字节/像素，而 buffer 格式是系统挑的（NativeActivity
    // 不指定时模拟器/部分设备会给 RGB_565），所以先钉成 RGBA_8888；锁不到
    // 32bpp 就丢帧 —— 按 565 的行距写 4bpp 会直接越出映射尾部。
    if (ANativeWindow_getFormat(nativeWindow) != WINDOW_FORMAT_RGBA_8888) {
        ANativeWindow_setBuffersGeometry(nativeWindow, 0, 0, WINDOW_FORMAT_RGBA_8888);
    }

    void* pixmapAddr = nullptr;
    int rowBytes = 0, pixmapW = 0, pixmapH = 0;
    if (!renderer_->peekPixels(&pixmapAddr, &rowBytes, &pixmapW, &pixmapH))
        return;
    if (rowBytes != blitW * 4) {
        __android_log_print(ANDROID_LOG_ERROR, "cui_core",
                            "unexpected Skia stride %d (expected %d)", rowBytes, blitW * 4);
        return;
    }

    ANativeWindow_Buffer buffer{};
    if (ANativeWindow_lock(nativeWindow, &buffer, nullptr) != 0)
        return;
    if (buffer.format != WINDOW_FORMAT_RGBA_8888 && buffer.format != WINDOW_FORMAT_RGBX_8888) {
        __android_log_print(ANDROID_LOG_ERROR, "cui_core",
                            "unsupported buffer format %d (%dx%d stride=%d)", buffer.format,
                            buffer.width, buffer.height, buffer.stride);
        ANativeWindow_unlockAndPost(nativeWindow);
        return;
    }
    const auto* src = static_cast<const uint8_t*>(pixmapAddr);
    auto* dst = static_cast<uint8_t*>(buffer.bits);
    const int copyW = std::min(blitW, buffer.width);
    // buffer.stride 是**像素**行距（不是字节），所以行偏移要 ×4；行数取
    // 窗口与 buffer 的小值，避免 surface 刚 resize 完、Skia 表面还是旧尺寸时越界。
    const int copyH = std::min(blitH, buffer.height);
    for (int y = 0; y < copyH; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * buffer.stride * 4,
                    src + static_cast<size_t>(y) * rowBytes,
                    static_cast<size_t>(copyW) * 4);
    }
    ANativeWindow_unlockAndPost(nativeWindow);
    presentDamageAll_ = true;

#elif defined(__OHOS__)
    // 鸿蒙的呈现整段在 OhosWindow 里（native buffer 队列的 Request/Flush 配对是
    // 窗口对象的私有状态，放不进这里）。这一支也顺手挡住了下面
    // `#elif defined(__linux__)` —— OHOS 的 triple 是 aarch64-linux-ohos，
    // `__linux__` 同样为 1。
    (void)handles;
    void* ohosAddr = nullptr;
    int ohosRowBytes = 0, ohosW = 0, ohosH = 0;
    if (!renderer_->peekPixels(&ohosAddr, &ohosRowBytes, &ohosW, &ohosH))
        return;
    if (platformWindow_->presentPixels(ohosAddr, ohosRowBytes, 0, 0, platformWindow_->width(),
                                       platformWindow_->height()))
        presentDamageAll_ = true;
#elif defined(__APPLE__) && defined(__MACH__) && TARGET_OS_IPHONE
    // iOS 软件上屏：像素由 IOSWindow::presentPixels 画到 UIView 的 CALayer
    // （与 OHOS 支同构）。iOS 没有 X11/Win32 那种系统图形 API，Skia 也不在
    // UIView 上开 GPU Surface，所以走这条 CPU 光栅 + 位图拷贝的路径。
    (void)handles;
    void* iosAddr = nullptr;
    int iosRowBytes = 0, iosW = 0, iosH = 0;
    if (!renderer_->peekPixels(&iosAddr, &iosRowBytes, &iosW, &iosH))
        return;
    if (platformWindow_->presentPixels(iosAddr, iosRowBytes, 0, 0,
                                       platformWindow_->width(),
                                       platformWindow_->height()))
        presentDamageAll_ = true;
#elif defined(__linux__)
    auto* conn = static_cast<xcb_connection_t*>(handles.xcb_connection);
    auto gc = static_cast<xcb_gcontext_t>(handles.xcb_gc);
    if (!conn || !gc)
        return;

    int blitW = winWidth_;
    int blitH = winHeight_;
    blitW = platformWindow_->width();
    blitH = platformWindow_->height();

    const auto depth = static_cast<uint8_t>(handles.xcb_depth ? handles.xcb_depth : 32);

    int rx = 0, ry = 0, rw = blitW, rh = blitH;
    if (!presentDamageAll_) {
        long x0 = static_cast<long>(std::floor(presentDamage_.x));
        long y0 = static_cast<long>(std::floor(presentDamage_.y));
        long x1 = static_cast<long>(std::ceil(presentDamage_.x + presentDamage_.width));
        long y1 = static_cast<long>(std::ceil(presentDamage_.y + presentDamage_.height));
        x0 = std::max(0L, std::min(x0, static_cast<long>(blitW)));
        y0 = std::max(0L, std::min(y0, static_cast<long>(blitH)));
        x1 = std::max(0L, std::min(x1, static_cast<long>(blitW)));
        y1 = std::max(0L, std::min(y1, static_cast<long>(blitH)));
        if (x1 > x0 && y1 > y0) {
            rx = static_cast<int>(x0);
            ry = static_cast<int>(y0);
            rw = static_cast<int>(x1 - x0);
            rh = static_cast<int>(y1 - y0);
        }
    }
    const bool full = (rx == 0 && ry == 0 && rw == blitW && rh == blitH);

    void* pixmapAddr = nullptr;
    int rowBytes = 0, pixmapW = 0, pixmapH = 0;
    if (renderer_->peekPixels(&pixmapAddr, &rowBytes, &pixmapW, &pixmapH)) {
        if (rowBytes != blitW * 4) {
            std::fprintf(stderr, "cui_core: unexpected Skia stride %d (expected %d)\n", rowBytes, blitW * 4);
            return;
        }
        const uint8_t* src = static_cast<const uint8_t*>(pixmapAddr);
        if (full) {
            xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, handles.xcb_window, gc,
                          static_cast<uint16_t>(blitW), static_cast<uint16_t>(blitH),
                          0, 0, 0, depth,
                          static_cast<uint32_t>(rowBytes * blitH), src);
        } else {
            const size_t stride = static_cast<size_t>(rw) * 4;
            if (blitScratch_.size() < stride * static_cast<size_t>(rh))
                blitScratch_.resize(stride * static_cast<size_t>(rh));
            for (int y = 0; y < rh; ++y)
                std::memcpy(blitScratch_.data() + static_cast<size_t>(y) * stride,
                            src + static_cast<size_t>(ry + y) * rowBytes + static_cast<size_t>(rx) * 4,
                            stride);
            xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, handles.xcb_window, gc,
                          static_cast<uint16_t>(rw), static_cast<uint16_t>(rh),
                          static_cast<uint16_t>(rx), static_cast<uint16_t>(ry), 0, depth,
                          static_cast<uint32_t>(stride * static_cast<size_t>(rh)),
                          blitScratch_.data());
        }
    } else {
        const size_t need = static_cast<size_t>(rw) * static_cast<size_t>(rh) * 4;
        if (readbackBuf_.size() != need)
            readbackBuf_.resize(need);
        if (!renderer_->readPixels(readbackBuf_.data(), rw, rh, rx, ry)) {
            std::fprintf(stderr, "cui_core: GPU readPixels failed (%dx%d@%d,%d)\n", rw, rh, rx, ry);
            return;
        }
        xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, handles.xcb_window, gc,
                      static_cast<uint16_t>(rw), static_cast<uint16_t>(rh),
                      static_cast<uint16_t>(rx), static_cast<uint16_t>(ry), 0, depth,
                      static_cast<uint32_t>(need), readbackBuf_.data());
    }
    xcb_flush(conn);
    presentDamageAll_ = true;

#elif defined(_WIN32)
    auto* hdc = static_cast<HDC>(handles.hdc);
    if (!hdc)
        return;
    // nativeHandles() 每帧 GetDC 一次，所以每条退出路径都必须 ReleaseDC。
    struct DcGuard {
        HWND hwnd;
        HDC dc;
        ~DcGuard() { ReleaseDC(hwnd, dc); }
    } dcGuard{static_cast<HWND>(handles.hwnd), hdc};
    int blitW = winWidth_;
    int blitH = winHeight_;
    blitW = platformWindow_->width();
    blitH = platformWindow_->height();

    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih);
    bih.biWidth = blitW;
    bih.biHeight = -blitH;  // top-down DIB
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    void* pixmapAddr = nullptr;
    int rowBytes = 0, pixmapW = 0, pixmapH = 0;
    const bool peeked = renderer_->peekPixels(&pixmapAddr, &rowBytes, &pixmapW, &pixmapH);
    if (peeked) {
        if (rowBytes != blitW * 4) {
            std::fprintf(stderr, "cui_core: unexpected Skia stride %d (expected %d)\n",
                         rowBytes, blitW * 4);
            return;
        }
        SetDIBitsToDevice(hdc, 0, 0, static_cast<DWORD>(blitW), static_cast<DWORD>(blitH),
                          0, 0, 0, static_cast<DWORD>(blitH), pixmapAddr,
                          reinterpret_cast<BITMAPINFO*>(&bih), DIB_RGB_COLORS);
    } else {
        const size_t need = static_cast<size_t>(blitW) * static_cast<size_t>(blitH) * 4;
        if (readbackBuf_.size() != need)
            readbackBuf_.resize(need);
        if (!renderer_->readPixels(readbackBuf_.data(), blitW, blitH))
            return;
        SetDIBitsToDevice(hdc, 0, 0, static_cast<DWORD>(blitW), static_cast<DWORD>(blitH),
                          0, 0, 0, static_cast<DWORD>(blitH), readbackBuf_.data(),
                          reinterpret_cast<BITMAPINFO*>(&bih), DIB_RGB_COLORS);
    }
#elif defined(__EMSCRIPTEN__)
    int blitW = winWidth_;
    int blitH = winHeight_;
    if (blitW <= 0 || blitH <= 0)
        return;

    int rx = 0, ry = 0, rw = blitW, rh = blitH;
    if (!presentDamageAll_) {
        const int x0 = std::max(0, std::min(static_cast<int>(std::floor(presentDamage_.x)), blitW));
        const int y0 = std::max(0, std::min(static_cast<int>(std::floor(presentDamage_.y)), blitH));
        const int x1 = std::max(0, std::min(static_cast<int>(std::ceil(
                                            presentDamage_.x + presentDamage_.width)), blitW));
        const int y1 = std::max(0, std::min(static_cast<int>(std::ceil(
                                            presentDamage_.y + presentDamage_.height)), blitH));
        if (x1 > x0 && y1 > y0) {
            rx = x0;
            ry = y0;
            rw = x1 - x0;
            rh = y1 - y0;
        }
    }

    void* pixmapAddr = nullptr;
    int rowBytes = 0, pixmapW = 0, pixmapH = 0;
    if (renderer_->peekPixels(&pixmapAddr, &rowBytes, &pixmapW, &pixmapH)) {
        if (rowBytes != blitW * 4) {
            std::fprintf(stderr, "cui_core: unexpected Skia stride %d (expected %d)\n",
                         rowBytes, blitW * 4);
            return;
        }
        const int maxW = std::min(blitW, pixmapW);
        const int maxH = std::min(blitH, pixmapH);
        if (rx >= maxW || ry >= maxH)
            return;
        rw = std::min(rw, maxW - rx);
        rh = std::min(rh, maxH - ry);
        // peekPixels 给的是**整帧**首地址；presentPixelsToCanvas 把入参当脏区
        // 左上角逐行读，所以必须先偏移到 (rx, ry)，否则会把画面左上角的内容
        // 错贴到脏区位置（症状：Ribbon/菜单被叠加到页面中部，动画页鬼影）。
        const uint8_t* rectBase = static_cast<const uint8_t*>(pixmapAddr) +
                                  static_cast<size_t>(ry) * static_cast<size_t>(rowBytes) +
                                  static_cast<size_t>(rx) * 4;
        presentPixelsToCanvas(rectBase, rowBytes, rx, ry, rw, rh);
    } else {
        const size_t need = static_cast<size_t>(rw) * static_cast<size_t>(rh) * 4;
        if (readbackBuf_.size() != need)
            readbackBuf_.resize(need);
        if (!renderer_->readPixels(readbackBuf_.data(), rw, rh, rx, ry))
            return;
        presentPixelsToCanvas(readbackBuf_.data(), rw * 4, rx, ry, rw, rh);
    }
    presentDamageAll_ = true;
#endif
}

// ── input ───────────────────────────────────────────────────────────────────

namespace {
PointerButtons buttonMaskFor(int button) {
    switch (button) {
        case 1: return kPrimaryMouseButton;
        case 2: return kMiddleMouseButton;
        case 3: return kSecondaryMouseButton;
        default: return 0;
    }
}
}  // namespace

void AppWindow::dispatchInput(const PlatformEvent& event) {
    if (!pipelineOwner_ || !buildOwner_)
        return;

    Offset p{event.pos.x, event.pos.y};

    switch (event.type) {
        case EventType::MouseDown: {
            PointerEvent ev;
            ev.type = PointerEventType::Down;
            ev.pointerKind = PointerDeviceKind::mouse;
            ev.pointerId = 1;
            ev.modifiers = event.modifiers;
            ev.position = p;
            ev.globalPosition = p;
            activeButtons_ |= buttonMaskFor(event.button);
            ev.buttons = activeButtons_;
            pipelineOwner_->dispatchPointerEvent(ev);
            scheduleRender();
            return;
        }
        case EventType::MouseUp: {
            PointerEvent ev;
            ev.type = PointerEventType::Up;
            ev.pointerKind = PointerDeviceKind::mouse;
            ev.pointerId = 1;
            ev.modifiers = event.modifiers;
            ev.position = p;
            ev.globalPosition = p;
            activeButtons_ &= ~buttonMaskFor(event.button);
            ev.buttons = activeButtons_;
            pipelineOwner_->dispatchPointerEvent(ev);
            scheduleRender();
            return;
        }
        case EventType::MouseMove: {
            PointerEvent ev;
            // 上游语义：鼠标无按键移动 = 悬停，有按键移动 = 拖拽。
            ev.type = activeButtons_ == 0 ? PointerEventType::Hover
                                         : PointerEventType::Move;
            ev.pointerKind = PointerDeviceKind::mouse;
            ev.pointerId = 1;
            ev.modifiers = event.modifiers;
            ev.position = p;
            ev.globalPosition = p;
            ev.buttons = activeButtons_;
            pipelineOwner_->dispatchPointerEvent(ev);
            scheduleRender();
            return;
        }
        case EventType::MouseWheel: {
            PointerEvent ev;
            ev.type = PointerEventType::Scroll;
            ev.pointerKind = PointerDeviceKind::mouse;
            ev.pointerId = 1;
            ev.modifiers = event.modifiers;
            ev.position = p;
            ev.globalPosition = p;
            ev.buttons = activeButtons_;
            const double scale =
                    event.wheelInNotches ? app_.wheelPixelsPerNotch() : 1.0;
            ev.scrollDeltaX = event.deltaX * scale;
            ev.scrollDeltaY = event.deltaY * scale;
            pipelineOwner_->dispatchPointerEvent(ev);
            scheduleRender();
            return;
        }
        case EventType::KeyDown: {
            // Tab/Shift+Tab：焦点遍历。EditableText 对 Tab 返回 Ignored。
            if (event.key == static_cast<int>(KeyCode::Tab)) {
                buildOwner_->focusManager().nextFocus(
                    (event.modifiers & ModShift) != 0);
                scheduleRender();
                return;
            }
            if (auto* fn = buildOwner_->focusManager().primaryFocus()) {
                KeyEvent ke{event.key, event.text, event.modifiers, false};
                buildOwner_->focusManager().sendKeyEvent(ke);
            }
            scheduleRender();
            return;
        }
        case EventType::Preedit: {
            if (auto* fn = buildOwner_->focusManager().primaryFocus())
                if (fn->onPreedit)
                    fn->onPreedit(event.text);
            scheduleRender();
            return;
        }
        default:
            return;
    }
}

}  // namespace cui_core
