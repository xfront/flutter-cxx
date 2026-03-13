/// @file skia_renderer.h
/// @brief Skia rendering backend — encapsulates all GPU/raster rendering logic.

#pragma once

#include <include/core/SkPicture.h>
#include <include/core/SkSurface.h>
#include <memory>
#include <vector>

#include "../foundation/geometry.h"

#if CUI_SKIA_GRAPHITE_VULKAN
#include <vulkan/vulkan.h>
#include <include/gpu/graphite/BackendTexture.h>
#endif

#if CUI_SKIA_GRAPHITE
namespace skgpu::graphite {
class Context;
class Recorder;
}  // namespace skgpu::graphite
#endif

struct xcb_connection_t;
typedef uint32_t xcb_window_t;

namespace cui {

/// Skia rendering backend — manages surface creation, GPU context, and blitting.
/// Encapsulates all Skia-specific logic (Vulkan/Metal/Dawn initialization,
/// memory allocators, ImageProvider) to keep Application clean.
class SkiaRenderer {
public:
    SkiaRenderer();
    ~SkiaRenderer();

    /// Initialize the renderer for the given window dimensions.
    /// Returns true on success, false if Skia initialization fails.
    bool init(int width, int height);

    /// Initialize with XCB window for Vulkan swapchain (Linux).
    /// This enables true GPU acceleration via direct presentation.
    bool initWithXcbWindow(xcb_connection_t* connection, xcb_window_t window, int width, int height);

    /// Resize the rendering surface and swapchain.
    void resize(int width, int height);

    /// Begin a new frame — acquires the next swapchain image.
    /// Returns true if a frame was acquired successfully.
    bool beginFrame();

    /// Ensure swapchain matches current dimensions (recreate if dirty).
    void ensureSwapchain();

    /// End the current frame — presents the swapchain image.
    void endFrame();

    /// Get the SkCanvas for drawing.
    SkCanvas* canvas() const;

    /// Submit recorded GPU work and flush (Graphite only).
    void flush();

    /// Read pixels from the surface into a CPU buffer.
    /// Returns true on success.
    bool readPixels(void* pixels, int width, int height, int srcX = 0, int srcY = 0);

    /// Rasterize an SkPicture into a GPU-backed SkImage (one texture).
    /// The picture is drawn at its cull rect's origin, so the returned image
    /// is positioned by the caller with drawImage(image, cull.x, cull.y).
    /// Returns null when no GPU backend is active or the picture is too big
    /// to bake; callers must keep the drawPicture fallback for that case.
    sk_sp<SkImage> rasterizePicture(SkPicture* pic);

    /// Try to peek pixels directly (works for raster surfaces).
    /// Returns true if peekPixels succeeded.
    bool peekPixels(void** addr, int* rowBytes, int* width, int* height);

    /// Current surface dimensions.
    int width() const { return width_; }
    int height() const { return height_; }

    /// Whether GPU acceleration is active.
    bool isGpuAccelerated() const;

#if CUI_SKIA_GRAPHITE
    /// Access the Graphite context (for advanced use).
    skgpu::graphite::Context* graphiteContext() const { return graphiteContext_.get(); }
    skgpu::graphite::Recorder* graphiteRecorder() const { return graphiteRecorder_.get(); }
#endif

private:
    int width_ = 0;
    int height_ = 0;
    sk_sp<SkSurface> surface_;
    bool swapchainMode_ = false;  // True when using Vulkan swapchain

#if CUI_SKIA_GRAPHITE_VULKAN
    struct VulkanHandles;
    std::unique_ptr<VulkanHandles> vulkanHandles_;
    
    // Stored XCB handles for swapchain recreation on resize
    xcb_connection_t* xcbConnection_ = nullptr;
    xcb_window_t xcbWindow_ = 0;
    bool swapchainDirty_ = false;  // True when swapchain needs recreation (deferred from resize);

    /// Initialize Vulkan swapchain for the given XCB window.
    bool initVulkanSwapchain(xcb_connection_t* connection, xcb_window_t window, int width, int height);

    /// Recreate the swapchain (e.g., after resize).
    bool recreateSwapchain(int width, int height);

    /// Acquire the next swapchain image.
    bool acquireNextSwapchainImage();

    /// Present the current swapchain image.
    bool presentSwapchainImage();

    /// Create the persistent offscreen drawing buffer (上游-style).
    /// EVERY frame paints into this one target; endFrame() copies it into
    /// whichever swapchain image was acquired.
    bool createOffscreenTarget();

    /// Destroy the offscreen drawing buffer (must be called before
    /// vulkanHandles_ is destroyed).  Leaves the buffer without content, so
    /// the next frame must be a full one.
    void destroyOffscreenTarget();

    /// surface_（Graphite RenderTarget）背后的 VkImage / VulkanTexture，
    /// 供 readPixels 的原生 vkCmdCopyImageToBuffer 回退使用。
    /// 非拥有指针：生命周期与 surface_ 一致。
    VkImage gpuVkImage_ = VK_NULL_HANDLE;
    void* gpuVkTexture_ = nullptr;

    /// Skia readPixels 被规划层拒绝时的原生 Vulkan 读回。
    bool nativeReadPixels(void* pixels, int width, int height, int srcX, int srcY);

    /// Destroy swapchain-level resources while keeping Vulkan device/Graphite context.
    /// Called before recreating the swapchain on resize.
    void destroySwapchainResources();

    /// Find a suitable memory type for Vulkan allocation.
    uint32_t findMemoryType(uint32_t typeBits, uint32_t props) const;

    uint32_t currentSwapchainImageIndex_ = 0;

    /// The single persistent drawing buffer.  Painting per-swapchain-image
    /// instead (one target per image, as the first cut did) breaks
    /// incremental frames: an animation frame only repaints its damage
    /// region, so the images that did not receive the last FULL frame keep
    /// undefined pixels everywhere else — a black band across the window that
    /// flickers in as the swapchain rotates (right after startup or a resize,
    /// when a swapchain recreation wipes every target).
    struct OffscreenTarget {
        VkImage vkImage = VK_NULL_HANDLE;  // Extracted from Graphite's internal texture
        // Raw pointer to Graphite's VulkanTexture (non-owning; lifetime tied to
        // `surface`).  Used to query the CURRENT image layout Graphite tracks —
        // our manual blit must start from the real layout, not an assumed one.
        void* vkTexture = nullptr;
        sk_sp<SkSurface> surface;          // Graphite-owned SkSurface
    };
    OffscreenTarget offscreen_;
#endif

#if CUI_SKIA_GRAPHITE_DAWN
    struct DawnHandles;
    std::unique_ptr<DawnHandles> dawnHandles_;
#endif

#if CUI_SKIA_GRAPHITE_METAL
    struct MetalHandles;
    std::unique_ptr<MetalHandles> metalHandles_;
#endif

#if CUI_SKIA_GRAPHITE
    std::unique_ptr<skgpu::graphite::Context> graphiteContext_;
    std::unique_ptr<skgpu::graphite::Recorder> graphiteRecorder_;
#endif

    /// Create a raster (CPU) surface.
    bool createRasterSurface(int width, int height);

#if CUI_SKIA_GRAPHITE
    /// Create a GPU-backed surface.
    /// @param skipReadPixelsTest If true, skip the readPixels test (for swapchain mode).
    bool createGpuSurface(int width, int height, bool skipReadPixelsTest = false);
#endif

#if CUI_SKIA_GRAPHITE_VULKAN
    /// Initialize Vulkan backend.
    bool initVulkan();
#endif

#if CUI_SKIA_GRAPHITE_DAWN
    /// Initialize Dawn/WebGPU backend.
    bool initDawn();
#endif

#if CUI_SKIA_GRAPHITE_METAL
    /// Initialize Metal backend.
    bool initMetal();
#endif
};

}  // namespace cui_core
