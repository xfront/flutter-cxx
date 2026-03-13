/// @file skia_renderer.cpp
/// @brief Skia rendering backend implementation — GPU context, surface management, blitting.

#include "cui/rendering/skia_renderer.h"

#include <include/core/SkBitmap.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkSurface.h>
#include <include/encode/SkPngEncoder.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#if CUI_SKIA_GRAPHITE
#include <include/gpu/graphite/Context.h>
#include <include/gpu/graphite/ContextOptions.h>
#include <include/gpu/graphite/Image.h>
#include <include/gpu/graphite/ImageProvider.h>
#include <include/gpu/graphite/Recorder.h>
#include <include/gpu/graphite/Surface.h>
#endif

#if CUI_SKIA_GRAPHITE_DAWN
#include <include/gpu/graphite/dawn/DawnBackendContext.h>
#include <webgpu/webgpu_cpp.h>
#endif

#if CUI_SKIA_GRAPHITE_VULKAN
#include <include/gpu/vk/VulkanBackendContext.h>
#include <include/gpu/vk/VulkanExtensions.h>
#include <include/gpu/vk/VulkanPreferredFeatures.h>
#include <include/gpu/graphite/vk/VulkanGraphiteContext.h>
#include <include/gpu/graphite/vk/VulkanGraphiteTypes.h>
#include <include/gpu/graphite/BackendTexture.h>
// Internal Graphite headers for extracting VkImage from GPU surfaces
#include <src/gpu/graphite/Surface_Graphite.h>
#include <src/gpu/graphite/TextureProxy.h>
#include <src/gpu/graphite/TextureProxyView.h>
#include <src/gpu/graphite/vk/VulkanTexture.h>
#include <xcb/xcb.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xcb.h>
#endif

#if CUI_SKIA_GRAPHITE_METAL
#include <include/gpu/graphite/mtl/MtlBackendContext.h>
#endif

namespace cui {

// ── ImageProvider for CPU→GPU conversion ────────────────────────────────────

#if CUI_SKIA_GRAPHITE
/// Graphite 只采样自己纹理里的图：内置的 DefaultImageProvider 刻意不做任何
/// 转换，所以「CPU 位图直接 drawImage」整条会被丢掉（Linux/Vulkan 上图像页
/// 全空白，stderr 刷 "Couldn't convert SkImage to a Graphite-backed
/// representation" + "Key context creation failed … draw dropped"）。
/// 上传交给 SkImages::TextureFromImage：它按当前后端的 caps 挑可上传/可渲染的
/// 纹理格式，返回图天然满足 Skia 侧 valid_client_provided_image 的尺寸、
/// alphaType、通道超集三条契约。自己拼 RenderTarget + writePixels 反而会在
/// 该 colorType 不可渲染的驱动上直接拿到空 surface。
class CpuToGpuImageProvider : public skgpu::graphite::ImageProvider {
public:
    sk_sp<SkImage> findOrCreate(skgpu::graphite::Recorder* recorder,
                                const SkImage* image,
                                SkImage::RequiredProperties props) override {
        if (!recorder || !image)
            return nullptr;
        // 已经是 Graphite 纹理：直接引用，别再拷一份子集。
        if (image->isTextureBacked())
            return sk_ref_sp(const_cast<SkImage*>(image));

        // 缓存键必须带对象指针：SHARED 构建里每个 .so 都静态链了一份 Skia，
        // 各副本的 uniqueID 计数器互相独立，来自不同 .so 的图会撞号（例如
        // video 包的视频帧与外壳图标同为 id=2）。只按 uniqueID 索引会把
        // 30x23 图标的纹理发给 480x270 的视频帧，Graphite 的
        // valid_client_provided_image 尺寸契约不通过 ⇒ "Couldn't convert …
        // draw dropped"，画面全空。命中时再校验 id / 尺寸 / alphaType / mipmap
        // 要求，防止对象地址被复用后误命中旧纹理。
        const CacheValue* hit = nullptr;
        const auto it = cache_.find(image);
        if (it != cache_.end()) {
            const CacheValue& v = it->second;
            if (v.uniqueID == image->uniqueID() && v.width == image->width() &&
                v.height == image->height() && v.alphaType == image->alphaType() &&
                v.props == props)
                hit = &v;
        }
        if (hit)
            return hit->gpu;

        sk_sp<SkImage> gpu = SkImages::TextureFromImage(recorder, image, props);
        if (!gpu)
            return nullptr;
        if (cache_.size() >= kCacheCap)
            cache_.clear();
        cache_.insert_or_assign(image, CacheValue{image->uniqueID(), image->width(),
                                                  image->height(), image->alphaType(),
                                                  props, gpu});
        return gpu;
    }

private:
    struct CacheValue {
        uint32_t uniqueID;
        int width;
        int height;
        SkAlphaType alphaType;
        SkImage::RequiredProperties props;
        sk_sp<SkImage> gpu;
    };
    static constexpr size_t kCacheCap = 256;
    std::unordered_map<const SkImage*, CacheValue> cache_;
};
#endif

// ── Vulkan memory allocator ──────────────────────────────────────────────────

#if CUI_SKIA_GRAPHITE_VULKAN

/// Minimal VulkanMemoryAllocator: allocates VkDeviceMemory directly using
/// vkGetPhysicalDeviceMemoryProperties to find a suitable memory type.
/// Skia requires a VulkanMemoryAllocator; without one the context creation
/// fails silently or with validate() errors.
class SimpleVulkanAllocator : public skgpu::VulkanMemoryAllocator {
public:
    SimpleVulkanAllocator(VkPhysicalDevice physDev, VkDevice device)
        : physDev_(physDev), device_(device) {
        vkGetPhysicalDeviceMemoryProperties(physDev, &memProps_);
    }

    VkResult allocateImageMemory(VkImage image, uint32_t flags,
                                 skgpu::VulkanBackendMemory* memory) override {
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, image, &req);
        VkMemoryPropertyFlags props = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        if (flags & kPersistentlyMapped_AllocationPropertyFlag)
            props |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        return allocate(req, props, memory);
    }

    VkResult allocateBufferMemory(VkBuffer buffer, BufferUsage usage, uint32_t flags,
                                  skgpu::VulkanBackendMemory* memory) override {
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, buffer, &req);
        VkMemoryPropertyFlags props = 0;
        switch (usage) {
            case BufferUsage::kGpuOnly:
                props = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                break;
            case BufferUsage::kCpuWritesGpuReads:
                props = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                break;
            case BufferUsage::kTransfersFromCpuToGpu:
                props = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                break;
            case BufferUsage::kTransfersFromGpuToCpu:
                props = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
                break;
        }
        return allocate(req, props, memory);
    }

    void getAllocInfo(const skgpu::VulkanBackendMemory& backend,
                      skgpu::VulkanAlloc* out) const override {
        auto* a = reinterpret_cast<Alloc*>(backend);
        out->fMemory = a->memory;
        out->fOffset = 0;
        out->fSize = a->size;
        out->fFlags = a->flags;
        out->fBackendMemory = backend;
    }

    void* mapMemory(const skgpu::VulkanBackendMemory& backend) override {
        auto* a = reinterpret_cast<Alloc*>(backend);
        void* ptr = nullptr;
        if (vkMapMemory(device_, a->memory, 0, a->size, 0, &ptr) == VK_SUCCESS)
            return ptr;
        return nullptr;
    }

    void unmapMemory(const skgpu::VulkanBackendMemory& backend) override {
        auto* a = reinterpret_cast<Alloc*>(backend);
        vkUnmapMemory(device_, a->memory);
    }

    void freeMemory(const skgpu::VulkanBackendMemory& backend) override {
        auto* a = reinterpret_cast<Alloc*>(backend);
        vkFreeMemory(device_, a->memory, nullptr);
        delete a;
    }

    std::pair<uint64_t, uint64_t> totalAllocatedAndUsedMemory() const override {
        return {totalAllocated_.load(), totalAllocated_.load()};
    }

private:
    struct Alloc {
        VkDeviceMemory memory;
        VkDeviceSize size;
        uint32_t flags;
    };

    VkResult allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags props,
                      skgpu::VulkanBackendMemory* out) {
        uint32_t typeIndex = findMemoryType(req.memoryTypeBits, props);
        if (typeIndex == UINT32_MAX)
            return VK_ERROR_MEMORY_MAP_FAILED;
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = req.size;
        allocInfo.memoryTypeIndex = typeIndex;
        auto* a = new Alloc;
        a->size = req.size;
        a->flags = 0;
        if ((props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            !(props & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            a->flags = skgpu::VulkanAlloc::kNoncoherent_Flag;
        if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            a->flags |= skgpu::VulkanAlloc::kMappable_Flag;
        VkResult r = vkAllocateMemory(device_, &allocInfo, nullptr, &a->memory);
        if (r != VK_SUCCESS) {
            delete a;
            return r;
        }
        totalAllocated_ += req.size;
        *out = reinterpret_cast<skgpu::VulkanBackendMemory>(a);
        return VK_SUCCESS;
    }

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const {
        // First pass: exact match
        for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & props) == props)
                return i;
        }
        // Second pass: relax DEVICE_LOCAL requirement
        for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & props))
                return i;
        }
        return UINT32_MAX;
    }

    VkPhysicalDevice physDev_;
    VkDevice device_;
    VkPhysicalDeviceMemoryProperties memProps_;
    std::atomic<uint64_t> totalAllocated_{0};
};

struct SkiaRenderer::VulkanHandles {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily = 0;
    skgpu::VulkanExtensions extensions;
    VkPhysicalDeviceFeatures2 deviceFeatures2{};
    sk_sp<SimpleVulkanAllocator> memoryAllocator;

    // Swapchain support
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainImageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swapchainExtent = {0, 0};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    std::vector<VkFramebuffer> swapchainFramebuffers;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;
    std::vector<VkCommandBuffer> blitCommandBuffers;  // Pre-allocated for GPU copy (one per swapchain image)
    VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
    VkSemaphore renderFinishedSemaphore = VK_NULL_HANDLE;
    VkFence inFlightFence = VK_NULL_HANDLE;
    bool swapchainReady = false;

    ~VulkanHandles() {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);

            // Cleanup swapchain resources
            if (imageAvailableSemaphore != VK_NULL_HANDLE)
                vkDestroySemaphore(device, imageAvailableSemaphore, nullptr);
            if (renderFinishedSemaphore != VK_NULL_HANDLE)
                vkDestroySemaphore(device, renderFinishedSemaphore, nullptr);
            if (inFlightFence != VK_NULL_HANDLE)
                vkDestroyFence(device, inFlightFence, nullptr);
            if (commandPool != VK_NULL_HANDLE)
                vkDestroyCommandPool(device, commandPool, nullptr);
            for (auto fb : swapchainFramebuffers)
                vkDestroyFramebuffer(device, fb, nullptr);
            for (auto iv : swapchainImageViews)
                vkDestroyImageView(device, iv, nullptr);
            if (renderPass != VK_NULL_HANDLE)
                vkDestroyRenderPass(device, renderPass, nullptr);
            if (swapchain != VK_NULL_HANDLE)
                vkDestroySwapchainKHR(device, swapchain, nullptr);
            if (surface != VK_NULL_HANDLE)
                vkDestroySurfaceKHR(instance, surface, nullptr);

            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE)
            vkDestroyInstance(instance, nullptr);
    }
};
#endif

// ── Dawn/WebGPU handles ──────────────────────────────────────────────────────

#if CUI_SKIA_GRAPHITE_DAWN
struct SkiaRenderer::DawnHandles {
    wgpu::Instance instance;
    wgpu::Device device;
};
#endif

// ── Metal handles ────────────────────────────────────────────────────────────

#if CUI_SKIA_GRAPHITE_METAL
struct SkiaRenderer::MetalHandles {
    void* device = nullptr;       // id<MTLDevice>
    void* commandQueue = nullptr; // id<MTLCommandQueue>
};
#endif

// ── Implementation ───────────────────────────────────────────────────────────

SkiaRenderer::SkiaRenderer() = default;

SkiaRenderer::~SkiaRenderer() {
#if CUI_SKIA_GRAPHITE_VULKAN
    destroyOffscreenTarget();
#endif
    // 必须在成员析构前显式释放持有 Graphite 纹理的 surface：
    // 成员按声明逆序析构，surface_（声明在前）会排在 graphiteContext_ /
    // vulkanHandles_ 之后销毁——届时 VulkanInterface 已亡，纹理的
    // freeGpuData() 调到空函数指针直接 SIGSEGV。
    surface_.reset();
#if CUI_SKIA_GRAPHITE_VULKAN
    gpuVkTexture_ = nullptr;
    gpuVkImage_ = VK_NULL_HANDLE;
#endif
}

bool SkiaRenderer::init(int width, int height) {
    width_ = width;
    height_ = height;

    // CUI_FORCE_RASTER=1：跳过 GPU 后端，强制 CPU raster 渲染。
    // 用于排查 GPU 路径问题（如 Graphite readPixels 失败）或无 GPU 环境。
    if (std::getenv("CUI_FORCE_RASTER") != nullptr) {
        std::fprintf(stderr, "cui_core: CUI_FORCE_RASTER set — using raster surface\n");
        return createRasterSurface(width, height);
    }

#if defined(__EMSCRIPTEN__)
    // Browser WebGPU adapter/device acquisition is asynchronous, so Graphite
    // cannot be brought up synchronously here. Render into a raster surface
    // and let Application::blitToNativeWindow() copy it into the canvas.
    return createRasterSurface(width, height);
#endif

#if CUI_SKIA_GRAPHITE
    // Try GPU backends first
#if CUI_SKIA_GRAPHITE_VULKAN
    if (initVulkan()) {
        if (createGpuSurface(width, height)) {
            return true;
        }
        std::fprintf(stderr, "cui_core: Graphite/Vulkan surface creation failed, falling back\n");
    }
#endif
#if CUI_SKIA_GRAPHITE_DAWN
    if (initDawn()) {
        if (createGpuSurface(width, height)) {
            return true;
        }
        std::fprintf(stderr, "cui_core: Graphite/Dawn surface creation failed, falling back\n");
    }
#endif
#if CUI_SKIA_GRAPHITE_METAL
    if (initMetal()) {
        if (createGpuSurface(width, height)) {
            return true;
        }
        std::fprintf(stderr, "cui_core: Graphite/Metal surface creation failed, falling back\n");
    }
#endif
#endif  // CUI_SKIA_GRAPHITE

    // Fall back to raster
    return createRasterSurface(width, height);
}

bool SkiaRenderer::initWithXcbWindow(xcb_connection_t* connection, xcb_window_t window, int width, int height) {
    // dev-1（实测可正常 GPU 显示）的架构：GPU 渲染到 Graphite surface，
    // 呈现走 readPixels → xcb_put_image（CPU 回拷）。
    //
    // 此前的「Vulkan swapchain + 手工 blit」呈现在本机（Intel UHD 730,
    // Mesa ANV）实测不通：acquire/present/submit 全部返回 VK_SUCCESS，
    // offscreen 内容也确认在 GPU 上渲染成功，但屏幕永不更新——连官方
    // vkcube 在同一环境下都无法显示，属于环境级 WSI 呈现问题，非我们
    // 代码可修。故这里不再初始化 swapchain，统一走 init() 的
    // 「Graphite GPU surface + 软件呈现」路径（与 dev-1 一致）。
    // 这两个成员只在 Graphite/Vulkan 编译配置下存在（调用点也只在
    // __linux__ && CUI_SKIA_GRAPHITE_VULKAN 下），非 Vulkan 平台里本函数
    // 等价于 init()。
#if CUI_SKIA_GRAPHITE_VULKAN
    xcbConnection_ = connection;
    xcbWindow_ = window;
#else
    (void)connection;
    (void)window;
#endif
    return init(width, height);
}

void SkiaRenderer::resize(int width, int height) {
    if (width == width_ && height == height_)
        return;
    width_ = width;
    height_ = height;

#if CUI_SKIA_GRAPHITE
    if (graphiteContext_ && graphiteRecorder_) {
        createGpuSurface(width, height);
        return;
    }
#endif
    createRasterSurface(width, height);
}

void SkiaRenderer::ensureSwapchain() {
#if CUI_SKIA_GRAPHITE_VULKAN
    if (!swapchainDirty_ || !graphiteContext_ || !vulkanHandles_ || !xcbConnection_) return;
    swapchainDirty_ = false;
    recreateSwapchain(width_, height_);
#endif
}

SkCanvas* SkiaRenderer::canvas() const {
#if CUI_SKIA_GRAPHITE_VULKAN
    // Swapchain mode: always draw into the one persistent offscreen buffer.
    // It is NOT rotated with the swapchain, so the pixels a damage-only
    // (incremental) frame leaves alone are still the ones from the previous
    // frame — exactly the upstream's desktop `Surface`, whose GLX/EGL drawable
    // keeps its content across swaps (see "partial repaint" +
    // EGL_KHR_partial_update).
    if (vulkanHandles_ && vulkanHandles_->swapchainReady && offscreen_.surface) {
        return offscreen_.surface->getCanvas();
    }
#endif
    return surface_ ? surface_->getCanvas() : nullptr;
}

void SkiaRenderer::flush() {
#if CUI_SKIA_GRAPHITE
    if (graphiteContext_ && graphiteRecorder_) {
        auto recording = graphiteRecorder_->snap();
        if (recording) {
            skgpu::graphite::InsertRecordingInfo info;
            info.fRecording = recording.get();
            graphiteContext_->insertRecording(info);
            // Use kNo (async) in swapchain mode — Graphite and our blit copy
            // share the same Vulkan queue, so GPU execution order is guaranteed
            // by queue semantics.  The inFlightFence provides frame-to-frame
            // sync.  kYes would stall the CPU every frame waiting for GPU.
            graphiteContext_->submit(skgpu::graphite::SyncToCpu::kNo);
        }
    }
#endif
}

bool SkiaRenderer::readPixels(void* pixels, int width, int height, int srcX, int srcY) {
    // Read back from the drawing target (the upstream's `Surface::ReadPixels` reads
    // its own draw target for the same reason: only that one holds a complete
    // frame).
    sk_sp<SkSurface> src = surface_;
#if CUI_SKIA_GRAPHITE_VULKAN
    if (!src)
        src = offscreen_.surface;
#endif
    if (!src)
        return false;
    // Graphite defers everything: without this the readback races the
    // recording that produces the pixels being read.
    flush();
#if CUI_SKIA_GRAPHITE
    // flush() submits asynchronously (SyncToCpu::kNo) — the GPU may still be
    // executing when readPixels maps the backing texture, and the readback
    // silently fails.  Graphite 的 submit(SyncToCpu::kYes) 在没有挂起
    // recording 时会立即返回、不等待在途任务（"no active command buffer"
    // 警告即由此而来），因此这里再用 vkQueueWaitIdle 强制排空队列，
    // 保证纹理已被本轮命令写完后再映射读回。
    if (graphiteContext_ && graphiteContext_->hasPendingGPUWork())
        graphiteContext_->submit(skgpu::graphite::SyncToCpu::kYes);
#if CUI_SKIA_GRAPHITE_VULKAN
    if (vulkanHandles_ && vulkanHandles_->device != VK_NULL_HANDLE)
        vkQueueWaitIdle(vulkanHandles_->queue);
#endif
#endif
    if (src->readPixels(SkImageInfo::MakeN32Premul(width, height), pixels,
                        static_cast<size_t>(width) * 4, srcX, srcY))
        return true;
#if CUI_SKIA_GRAPHITE_VULKAN
    // Skia 的 readPixels 规划层在 Vulkan Graphite 集成下直接拒绝（返回
    // false，与 GPU 执行无关——原生读回验证过内容是正确的）。兜底走
    // vkCmdCopyImageToBuffer 原生读回。
    if (gpuVkImage_ != VK_NULL_HANDLE && vulkanHandles_ &&
        vulkanHandles_->device != VK_NULL_HANDLE)
        return nativeReadPixels(pixels, width, height, srcX, srcY);
#endif
    return false;
}

#if CUI_SKIA_GRAPHITE_VULKAN
bool SkiaRenderer::nativeReadPixels(void* pixels, int width, int height, int srcX, int srcY) {
    auto& vh = *vulkanHandles_;
    VkDevice dev = vh.device;
    if (gpuVkImage_ == VK_NULL_HANDLE || width <= 0 || height <= 0)
        return false;

    // 1. host-visible 暂存缓冲
    const VkDeviceSize bufSize = static_cast<VkDeviceSize>(width) * height * 4;
    VkBuffer buf = VK_NULL_HANDLE;
    VkBufferCreateInfo binfo{};
    binfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    binfo.size = bufSize;
    binfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    binfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &binfo, nullptr, &buf) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(vh.physicalDevice, &mp);
    uint32_t typeIdx = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
            typeIdx = i;
            break;
        }
    }
    if (typeIdx == UINT32_MAX) {
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                typeIdx = i;
                break;
            }
        }
    }
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (typeIdx != UINT32_MAX) {
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = typeIdx;
        if (vkAllocateMemory(dev, &ai, nullptr, &mem) != VK_SUCCESS ||
            vkBindBufferMemory(dev, buf, mem, 0) != VK_SUCCESS) {
            vkDestroyBuffer(dev, buf, nullptr);
            if (mem != VK_NULL_HANDLE)
                vkFreeMemory(dev, mem, nullptr);
            return false;
        }
    } else {
        vkDestroyBuffer(dev, buf, nullptr);
        return false;
    }

    // 2. 录制：按 Graphite 跟踪的真实 layout 做屏障 → copy → 还原 layout
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = vh.graphicsQueueFamily;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    bool ok = false;
    if (vkCreateCommandPool(dev, &pci, nullptr, &pool) == VK_SUCCESS) {
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(dev, &cai, &cmd) == VK_SUCCESS) {
            VkCommandBufferBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            auto* srcTex = static_cast<skgpu::graphite::VulkanTexture*>(gpuVkTexture_);
            // Graphite 提交前纹理处于其跟踪的 layout；flush() 已在
            // readPixels 入口完成（含 queueWaitIdle），此时读到的 layout
            // 就是当前真实 layout。
            VkImageLayout lay = srcTex ? srcTex->currentLayout()
                                       : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkImageMemoryBarrier bar{};
            bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.image = gpuVkImage_;
            bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bar.subresourceRange.levelCount = 1;
            bar.subresourceRange.layerCount = 1;
            bar.oldLayout = lay;
            bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            bar.srcAccessMask = 0;
            bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkBeginCommandBuffer(cmd, &bi);
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                0, nullptr, 0, nullptr, 1, &bar);
            VkBufferImageCopy copy{};
            copy.bufferRowLength = static_cast<uint32_t>(width);
            copy.bufferImageHeight = static_cast<uint32_t>(height);
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageOffset = {srcX, srcY, 0};
            copy.imageExtent = {static_cast<uint32_t>(width),
                                static_cast<uint32_t>(height), 1};
            vkCmdCopyImageToBuffer(cmd, gpuVkImage_,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &copy);
            bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            bar.newLayout = lay == VK_IMAGE_LAYOUT_UNDEFINED
                                ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : lay;
            bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            bar.dstAccessMask = 0;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                0, nullptr, 0, nullptr, 1, &bar);
            vkEndCommandBuffer(cmd);
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            if (vkQueueSubmit(vh.queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS) {
                vkQueueWaitIdle(vh.queue);
                void* ptr = nullptr;
                if (vkMapMemory(dev, mem, 0, bufSize, 0, &ptr) == VK_SUCCESS) {
                    // bufferRowLength=width → 行紧凑排布，与 dst 行距一致
                    std::memcpy(pixels, ptr, static_cast<size_t>(bufSize));
                    vkUnmapMemory(dev, mem);
                    ok = true;
                }
            }
        }
        vkFreeCommandBuffers(dev, pool, 1, &cmd);
    }
    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyBuffer(dev, buf, nullptr);
    vkFreeMemory(dev, mem, nullptr);
    return ok;
}
#endif

sk_sp<SkImage> SkiaRenderer::rasterizePicture(SkPicture* pic) {
    if (!pic)
        return nullptr;
#if CUI_SKIA_GRAPHITE
    // Same recorder as the offscreen frame target: the returned image is a
    // Graphite resource, drawable by later recordings on this recorder (the
    // bake recording is submitted before any recording that samples it).
    if (!graphiteRecorder_)
        return nullptr;
    const SkRect cull = pic->cullRect();
    SkIRect dim = cull.roundOut();
    // Refuse absurd sizes: a widget reporting a bogus paintBounds() would
    // otherwise allocate a giant texture.
    if (dim.isEmpty() || dim.width() > 4096 || dim.height() > 4096)
        return nullptr;
    auto surf = SkSurfaces::RenderTarget(
        graphiteRecorder_.get(),
        SkImageInfo::MakeN32Premul(dim.width(), dim.height()));
    if (!surf)
        return nullptr;
    SkCanvas* c = surf->getCanvas();
    c->clear(SK_ColorTRANSPARENT);
    c->translate(-cull.x(), -cull.y());
    c->drawPicture(pic);
    return surf->makeImageSnapshot();
#else
    (void)pic;
    return nullptr;
#endif
}

bool SkiaRenderer::peekPixels(void** addr, int* rowBytes, int* width, int* height) {
    if (!surface_)
        return false;
    auto image = surface_->makeImageSnapshot();
    SkPixmap pixmap;
    if (!image->peekPixels(&pixmap))
        return false;
    *addr = const_cast<void*>(pixmap.addr());
    *rowBytes = static_cast<int>(pixmap.rowBytes());
    *width = pixmap.width();
    *height = pixmap.height();
    return true;
}

bool SkiaRenderer::isGpuAccelerated() const {
#if CUI_SKIA_GRAPHITE_VULKAN
    // Swapchain mode: GPU-accelerated even though surface_ is null
    if (vulkanHandles_ && vulkanHandles_->swapchainReady && offscreen_.surface) {
        return true;
    }
#endif
#if CUI_SKIA_GRAPHITE
    // Check if we have a Graphite context AND a GPU-backed surface
    return graphiteContext_ != nullptr && surface_ && surface_->getCanvas();
#else
    return false;
#endif
}

bool SkiaRenderer::createRasterSurface(int width, int height) {
    // Clamp negative dimensions to prevent Skia crash
    if (width < 1 || height < 1) {
        std::fprintf(stderr, "cui_core: createRasterSurface WARNING: invalid dimensions %dx%d, clamping to 1x1\n", width, height);
        width = std::max(1, width);
        height = std::max(1, height);
    }
    surface_ = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    return surface_ != nullptr;
}

#if CUI_SKIA_GRAPHITE
bool SkiaRenderer::createGpuSurface(int width, int height, bool skipReadPixelsTest) {
    if (!graphiteRecorder_)
        return false;

    // Clamp negative dimensions to prevent Skia crash
    if (width < 1 || height < 1) {
        std::fprintf(stderr, "cui_core: createGpuSurface WARNING: invalid dimensions %dx%d, clamping to 1x1\n", width, height);
        width = std::max(1, width);
        height = std::max(1, height);
    }

    surface_ = SkSurfaces::RenderTarget(
        graphiteRecorder_.get(),
        SkImageInfo::MakeN32Premul(width, height));
    if (!surface_)
        return false;

#if CUI_SKIA_GRAPHITE_VULKAN
    gpuVkImage_ = VK_NULL_HANDLE;
    gpuVkTexture_ = nullptr;
    // 提取 surface 背后的 VkImage 与 Graphite 的 VulkanTexture：
    // Skia API 层的 readPixels 在这套集成里会被规划层直接拒绝
    // （返回 false），readPixels() 里改用原生 vkCmdCopyImageToBuffer
    // 读回 —— 实测能拿到正确的 GPU 内容。
    if (vulkanHandles_) {
        auto* gs = static_cast<skgpu::graphite::Surface*>(surface_.get());
        if (gs) {
            auto proxyView = gs->target();
            auto* proxy = proxyView.proxy();
            if (proxy && proxy->isInstantiated() && proxy->texture()) {
                auto* vkTex = static_cast<skgpu::graphite::VulkanTexture*>(proxy->texture());
                gpuVkImage_ = vkTex->vkImage();
                gpuVkTexture_ = vkTex;
            }
        }
    }
#endif

    // readPixels 是否可用不再一票否决 GPU 路径：呈现由原生回退兜底
    // （与 dev-1 的「GPU 渲染 + CPU 回拷呈现」架构一致）。
    (void)skipReadPixelsTest;
    std::fprintf(stderr, "cui_core: Graphite RenderTarget %dx%d\n", width, height);
    return true;
}
#endif

#if CUI_SKIA_GRAPHITE_VULKAN
bool SkiaRenderer::initVulkan() {
    // Reuse existing context if already initialized (resize uses recreateSwapchain instead)
    if (graphiteContext_ && vulkanHandles_) {
        return true;
    }

    vulkanHandles_ = std::make_unique<VulkanHandles>();
    auto& vh = *vulkanHandles_;

    // ── Vulkan initialisation following Skia's VulkanPreferredFeatures flow ──
    // Skia requires Vulkan 1.1+ and a specific set of extensions/features.
    // The VulkanPreferredFeatures helper negotiates the intersection of what
    // the device supports and what Skia needs.

    const uint32_t apiVersion = VK_API_VERSION_1_1;

    // 1. Initialise Skia's feature negotiation
    skgpu::VulkanPreferredFeatures skiaFeatures;
    skiaFeatures.init(apiVersion);

    // 2. Query instance extensions and let Skia add its own
    uint32_t instExtCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, nullptr);
    std::vector<VkExtensionProperties> instExts(instExtCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, instExts.data());
    std::vector<const char*> instExtNames;
    skiaFeatures.addToInstanceExtensions(instExts.data(), instExts.size(), instExtNames);

    // Always add surface extensions for swapchain support (if available)
    for (const char* surfExt : {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XCB_SURFACE_EXTENSION_NAME}) {
        bool found = false;
        for (const auto& ext : instExts) {
            if (strcmp(ext.extensionName, surfExt) == 0) {
                found = true;
                break;
            }
        }
        if (found) {
            // Check if not already added
            bool alreadyAdded = false;
            for (const char* existing : instExtNames) {
                if (strcmp(existing, surfExt) == 0) {
                    alreadyAdded = true;
                    break;
                }
            }
            if (!alreadyAdded) {
                instExtNames.push_back(surfExt);
            }
        }
    }

    // 3. Create Vulkan instance
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "cui_core";
    appInfo.apiVersion = apiVersion;
    VkInstanceCreateInfo instInfo{};
    instInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instInfo.pApplicationInfo = &appInfo;
    instInfo.enabledExtensionCount = static_cast<uint32_t>(instExtNames.size());
    instInfo.ppEnabledExtensionNames = instExtNames.empty() ? nullptr : instExtNames.data();

    if (vkCreateInstance(&instInfo, nullptr, &vh.instance) != VK_SUCCESS) {
        std::fprintf(stderr, "cui_core: vkCreateInstance failed\n");
        vulkanHandles_.reset();
        return false;
    }

    // 4. Pick the first physical device (prefer discrete GPU)
    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(vh.instance, &gpuCount, nullptr);
    if (gpuCount == 0) {
        std::fprintf(stderr, "cui_core: No Vulkan physical devices found\n");
        vulkanHandles_.reset();
        return false;
    }
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(vh.instance, &gpuCount, gpus.data());
    vh.physicalDevice = gpus[0];
    for (auto& gpu : gpus) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpu, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            vh.physicalDevice = gpu;
            break;
        }
    }

    // 无硬件 GPU 时放弃 Vulkan 路径（上游 同样在无 GPU 环境回退软件渲染）。
    // 软件设备（llvmpipe）本身就是 CPU 光栅化：走 Graphite→Vulkan→WSI 链路
    // 只会平添一层拷贝，且在这类环境（远程桌面/虚拟机）里 WSI 呈现经实测
    // 不可靠（连 vkcube 都无法正确显示）——不如直接用更快更稳的 CPU raster。
    // CUI_FORCE_GPU=1 可强制走 GPU 路径以便调试。
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(vh.physicalDevice, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU &&
            !std::getenv("CUI_FORCE_GPU")) {
            std::fprintf(stderr,
                         "cui_core: Vulkan device is software (%s) — using CPU raster "
                         "instead (set CUI_FORCE_GPU=1 to override)\n",
                         props.deviceName);
            vulkanHandles_.reset();
            return false;
        }
    }

    // 5. Find a graphics queue family
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vh.physicalDevice, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfProps(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(vh.physicalDevice, &qfCount, qfProps.data());
    for (uint32_t i = 0; i < qfCount; ++i) {
        if (qfProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            vh.graphicsQueueFamily = i;
            break;
        }
    }

    // 6. Query device extensions and let Skia add features/extensions
    uint32_t devExtCount = 0;
    vkEnumerateDeviceExtensionProperties(vh.physicalDevice, nullptr, &devExtCount, nullptr);
    std::vector<VkExtensionProperties> devExts(devExtCount);
    vkEnumerateDeviceExtensionProperties(vh.physicalDevice, nullptr, &devExtCount, devExts.data());

    // Query features with Skia's additions
    vh.deviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    skiaFeatures.addFeaturesToQuery(devExts.data(), devExts.size(), vh.deviceFeatures2);
    vkGetPhysicalDeviceFeatures2(vh.physicalDevice, &vh.deviceFeatures2);

    // Let Skia add its required extensions and features
    std::vector<const char*> devExtNames;
    skiaFeatures.addFeaturesToEnable(devExtNames, vh.deviceFeatures2);

    // Always add swapchain extension for swapchain support (if available)
    for (const auto& ext : devExts) {
        if (strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
            bool alreadyAdded = false;
            for (const char* existing : devExtNames) {
                if (strcmp(existing, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                    alreadyAdded = true;
                    break;
                }
            }
            if (!alreadyAdded) {
                devExtNames.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            }
            break;
        }
    }

    // 7. Create Vulkan device
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = vh.graphicsQueueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo devInfo{};
    devInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    devInfo.queueCreateInfoCount = 1;
    devInfo.pQueueCreateInfos = &queueInfo;
    devInfo.enabledExtensionCount = static_cast<uint32_t>(devExtNames.size());
    devInfo.ppEnabledExtensionNames = devExtNames.empty() ? nullptr : devExtNames.data();
    devInfo.pNext = &vh.deviceFeatures2;
    if (vkCreateDevice(vh.physicalDevice, &devInfo, nullptr, &vh.device) != VK_SUCCESS) {
        std::fprintf(stderr, "cui_core: vkCreateDevice failed\n");
        vulkanHandles_.reset();
        return false;
    }
    vkGetDeviceQueue(vh.device, vh.graphicsQueueFamily, 0, &vh.queue);

    // 8. Populate VulkanExtensions
    skgpu::VulkanGetProc getProc = [](const char* name, VkInstance inst, VkDevice dev) -> PFN_vkVoidFunction {
        if (dev != VK_NULL_HANDLE) {
            PFN_vkVoidFunction fn = vkGetDeviceProcAddr(dev, name);
            if (fn) return fn;
        }
        return vkGetInstanceProcAddr(inst, name);
    };
    vh.extensions.init(getProc, vh.instance, vh.physicalDevice,
                       static_cast<uint32_t>(instExtNames.size()),
                       instExtNames.empty() ? nullptr : instExtNames.data(),
                       static_cast<uint32_t>(devExtNames.size()),
                       devExtNames.empty() ? nullptr : devExtNames.data());

    // 9. Create memory allocator
    vh.memoryAllocator = sk_sp<SimpleVulkanAllocator>(
        new SimpleVulkanAllocator(vh.physicalDevice, vh.device));

    // 10. Create Graphite context with all required fields
    skgpu::VulkanBackendContext vkCtx;
    vkCtx.fInstance = vh.instance;
    vkCtx.fPhysicalDevice = vh.physicalDevice;
    vkCtx.fDevice = vh.device;
    vkCtx.fQueue = vh.queue;
    vkCtx.fGraphicsQueueIndex = vh.graphicsQueueFamily;
    vkCtx.fMaxAPIVersion = apiVersion;
    vkCtx.fVkExtensions = &vh.extensions;
    vkCtx.fDeviceFeatures2 = &vh.deviceFeatures2;
    vkCtx.fMemoryAllocator = vh.memoryAllocator;
    vkCtx.fGetProc = getProc;
    skgpu::graphite::ContextOptions opts;
    graphiteContext_ = skgpu::graphite::ContextFactory::MakeVulkan(vkCtx, opts);
    if (graphiteContext_) {
        skgpu::graphite::RecorderOptions recOpts;
        recOpts.fImageProvider = sk_make_sp<CpuToGpuImageProvider>();
        graphiteRecorder_ = graphiteContext_->makeRecorder(recOpts);
        std::fprintf(stderr, "cui_core: Graphite/Vulkan context created\n");
        return true;
    }

    std::fprintf(stderr, "cui_core: Graphite/Vulkan init failed, falling back\n");
    vulkanHandles_.reset();
    return false;
}

bool SkiaRenderer::initVulkanSwapchain(xcb_connection_t* connection, xcb_window_t window, int width, int height) {
    if (!vulkanHandles_) return false;
    auto& vh = *vulkanHandles_;

    // Check instance extensions for surface support
    uint32_t instExtCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, nullptr);
    std::vector<VkExtensionProperties> instExts(instExtCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, instExts.data());

    const char* requiredInstanceExtensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_XCB_SURFACE_EXTENSION_NAME
    };

    for (const char* reqExt : requiredInstanceExtensions) {
        bool found = false;
        for (const auto& ext : instExts) {
            if (strcmp(ext.extensionName, reqExt) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            std::fprintf(stderr, "cui_core: Required instance extension %s not supported\n", reqExt);
            return false;
        }
    }

    // Check device extensions for swapchain support
    uint32_t devExtCount = 0;
    vkEnumerateDeviceExtensionProperties(vh.physicalDevice, nullptr, &devExtCount, nullptr);
    std::vector<VkExtensionProperties> devExts(devExtCount);
    vkEnumerateDeviceExtensionProperties(vh.physicalDevice, nullptr, &devExtCount, devExts.data());

    const char* requiredDeviceExtensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };

    for (const char* reqExt : requiredDeviceExtensions) {
        bool found = false;
        for (const auto& ext : devExts) {
            if (strcmp(ext.extensionName, reqExt) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            std::fprintf(stderr, "cui_core: Required device extension %s not supported\n", reqExt);
            return false;
        }
    }

    // Create XCB surface
    VkXcbSurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.connection = connection;
    surfaceInfo.window = window;

    auto vkCreateXcbSurfaceKHR = reinterpret_cast<PFN_vkCreateXcbSurfaceKHR>(
        vkGetInstanceProcAddr(vh.instance, "vkCreateXcbSurfaceKHR"));
    if (!vkCreateXcbSurfaceKHR) {
        std::fprintf(stderr, "cui_core: vkCreateXcbSurfaceKHR not found\n");
        return false;
    }

    if (vkCreateXcbSurfaceKHR(vh.instance, &surfaceInfo, nullptr, &vh.surface) != VK_SUCCESS) {
        std::fprintf(stderr, "cui_core: vkCreateXcbSurfaceKHR failed\n");
        return false;
    }

    // Check surface support
    VkBool32 supported = VK_FALSE;
    auto vkGetPhysicalDeviceSurfaceSupportKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
        vkGetInstanceProcAddr(vh.instance, "vkGetPhysicalDeviceSurfaceSupportKHR"));
    vkGetPhysicalDeviceSurfaceSupportKHR(vh.physicalDevice, vh.graphicsQueueFamily, vh.surface, &supported);
    if (!supported) {
        std::fprintf(stderr, "cui_core: Surface not supported by queue family\n");
        return false;
    }

    // Get surface capabilities
    auto vkGetPhysicalDeviceSurfaceCapabilitiesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
        vkGetInstanceProcAddr(vh.instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vh.physicalDevice, vh.surface, &caps);

    // Choose swapchain format
    auto vkGetPhysicalDeviceSurfaceFormatsKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
        vkGetInstanceProcAddr(vh.instance, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vh.physicalDevice, vh.surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(vh.physicalDevice, vh.surface, &formatCount, formats.data());

    VkSurfaceFormatKHR chosenFormat = formats[0];
    for (const auto& fmt : formats) {
        if (fmt.format == VK_FORMAT_B8G8R8A8_UNORM && fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosenFormat = fmt;
            break;
        }
    }
    vh.swapchainImageFormat = chosenFormat.format;

    // Choose present mode
    auto vkGetPhysicalDeviceSurfacePresentModesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
        vkGetInstanceProcAddr(vh.instance, "vkGetPhysicalDeviceSurfacePresentModesKHR"));
    uint32_t presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vh.physicalDevice, vh.surface, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(vh.physicalDevice, vh.surface, &presentModeCount, presentModes.data());

    VkPresentModeKHR chosenPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (auto mode : presentModes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            chosenPresentMode = mode;
            break;
        }
    }

    // Determine swapchain extent
    VkExtent2D extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    vh.swapchainExtent = extent;

    // Determine image count
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    // Create swapchain
    VkSwapchainCreateInfoKHR swapchainInfo{};
    swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainInfo.surface = vh.surface;
    swapchainInfo.minImageCount = imageCount;
    swapchainInfo.imageFormat = chosenFormat.format;
    swapchainInfo.imageColorSpace = chosenFormat.colorSpace;
    swapchainInfo.imageExtent = extent;
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainInfo.queueFamilyIndexCount = 0;
    swapchainInfo.pQueueFamilyIndices = nullptr;
    swapchainInfo.preTransform = caps.currentTransform;
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainInfo.presentMode = chosenPresentMode;
    swapchainInfo.clipped = VK_TRUE;
    swapchainInfo.oldSwapchain = VK_NULL_HANDLE;

    auto vkCreateSwapchainKHR = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
        vkGetDeviceProcAddr(vh.device, "vkCreateSwapchainKHR"));
    if (vkCreateSwapchainKHR(vh.device, &swapchainInfo, nullptr, &vh.swapchain) != VK_SUCCESS) {
        std::fprintf(stderr, "cui_core: vkCreateSwapchainKHR failed\n");
        return false;
    }

    // Get swapchain images
    auto vkGetSwapchainImagesKHR = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
        vkGetDeviceProcAddr(vh.device, "vkGetSwapchainImagesKHR"));
    uint32_t swapchainImageCount;
    vkGetSwapchainImagesKHR(vh.device, vh.swapchain, &swapchainImageCount, nullptr);
    vh.swapchainImages.resize(swapchainImageCount);
    vkGetSwapchainImagesKHR(vh.device, vh.swapchain, &swapchainImageCount, vh.swapchainImages.data());

    // Create image views
    vh.swapchainImageViews.resize(swapchainImageCount);
    for (uint32_t i = 0; i < swapchainImageCount; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = vh.swapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = chosenFormat.format;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(vh.device, &viewInfo, nullptr, &vh.swapchainImageViews[i]) != VK_SUCCESS) {
            std::fprintf(stderr, "cui_core: Failed to create swapchain image view\n");
            return false;
        }
    }

    // Create render pass
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = chosenFormat.format;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;

    if (vkCreateRenderPass(vh.device, &renderPassInfo, nullptr, &vh.renderPass) != VK_SUCCESS) {
        std::fprintf(stderr, "cui_core: Failed to create render pass\n");
        return false;
    }

    // Create framebuffers
    vh.swapchainFramebuffers.resize(swapchainImageCount);
    for (uint32_t i = 0; i < swapchainImageCount; ++i) {
        VkImageView attachments[] = {vh.swapchainImageViews[i]};
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = vh.renderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = attachments;
        fbInfo.width = extent.width;
        fbInfo.height = extent.height;
        fbInfo.layers = 1;

        if (vkCreateFramebuffer(vh.device, &fbInfo, nullptr, &vh.swapchainFramebuffers[i]) != VK_SUCCESS) {
            std::fprintf(stderr, "cui_core: Failed to create framebuffer\n");
            return false;
        }
    }

    // Create command pool
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = vh.graphicsQueueFamily;

    if (vkCreateCommandPool(vh.device, &poolInfo, nullptr, &vh.commandPool) != VK_SUCCESS) {
        std::fprintf(stderr, "cui_core: Failed to create command pool\n");
        return false;
    }

    // Allocate command buffers
    vh.commandBuffers.resize(swapchainImageCount);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = vh.commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(swapchainImageCount);

    if (vkAllocateCommandBuffers(vh.device, &allocInfo, vh.commandBuffers.data()) != VK_SUCCESS) {
        std::fprintf(stderr, "cui_core: Failed to allocate command buffers\n");
        return false;
    }

    // Create semaphores and fence
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    if (vkCreateSemaphore(vh.device, &semaphoreInfo, nullptr, &vh.imageAvailableSemaphore) != VK_SUCCESS ||
        vkCreateSemaphore(vh.device, &semaphoreInfo, nullptr, &vh.renderFinishedSemaphore) != VK_SUCCESS ||
        vkCreateFence(vh.device, &fenceInfo, nullptr, &vh.inFlightFence) != VK_SUCCESS) {
        std::fprintf(stderr, "cui_core: Failed to create semaphores/fence\n");
        return false;
    }

    // Pre-allocate blit command buffers (one per swapchain image, reused every frame)
    vh.blitCommandBuffers.resize(swapchainImageCount);
    VkCommandBufferAllocateInfo blitCmdAlloc{};
    blitCmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    blitCmdAlloc.commandPool = vh.commandPool;
    blitCmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    blitCmdAlloc.commandBufferCount = static_cast<uint32_t>(swapchainImageCount);
    if (vkAllocateCommandBuffers(vh.device, &blitCmdAlloc, vh.blitCommandBuffers.data()) != VK_SUCCESS) {
        std::fprintf(stderr, "cui_core: Failed to allocate blit command buffers\n");
        return false;
    }

    vh.swapchainReady = true;
    std::fprintf(stderr, "cui_core: Vulkan swapchain created (%ux%u, %u images)\n",
                 extent.width, extent.height, swapchainImageCount);
    return true;
}

bool SkiaRenderer::recreateSwapchain(int width, int height) {
    if (!vulkanHandles_ || !graphiteContext_) return false;
    
    // Wait for GPU to finish before modifying resources
    auto& vh = *vulkanHandles_;
    if (vh.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(vh.device);
    }
    
    // Flush any pending Graphite work and reset the recorder
    if (graphiteRecorder_) {
        auto recording = graphiteRecorder_->snap();
        if (recording) {
            skgpu::graphite::InsertRecordingInfo info;
            info.fRecording = recording.get();
            graphiteContext_->insertRecording(info);
        }
        if (graphiteContext_->hasPendingGPUWork())
            graphiteContext_->submit(skgpu::graphite::SyncToCpu::kYes);
        graphiteRecorder_.reset();
    }
    
    // Destroy old swapchain-level resources (keeps Vulkan device + Graphite context)
    destroySwapchainResources();
    
    // Recreate swapchain using stored XCB handles
    if (!initVulkanSwapchain(xcbConnection_, xcbWindow_, width, height)) {
        std::fprintf(stderr, "cui_core: recreateSwapchain: initVulkanSwapchain failed\n");
        return false;
    }
    
    // Recreate the offscreen drawing buffer (creates a new recorder internally)
    if (!createOffscreenTarget()) {
        std::fprintf(stderr, "cui_core: recreateSwapchain: createOffscreenTarget failed\n");
        return false;
    }
    
    std::fprintf(stderr, "cui_core: Swapchain recreated (%dx%d)\n", width, height);
    return true;
}

bool SkiaRenderer::acquireNextSwapchainImage() {
    if (!vulkanHandles_ || !vulkanHandles_->swapchainReady) return false;
    auto& vh = *vulkanHandles_;

    auto vkAcquireNextImageKHR = reinterpret_cast<PFN_vkAcquireNextImageKHR>(
        vkGetDeviceProcAddr(vh.device, "vkAcquireNextImageKHR"));

    vkWaitForFences(vh.device, 1, &vh.inFlightFence, VK_TRUE, UINT64_MAX);
    vkResetFences(vh.device, 1, &vh.inFlightFence);

    VkResult result = vkAcquireNextImageKHR(vh.device, vh.swapchain, UINT64_MAX,
                                            vh.imageAvailableSemaphore, VK_NULL_HANDLE,
                                            &currentSwapchainImageIndex_);
    static int acquireLogs = 0;
    if (acquireLogs < 10) {
        std::fprintf(stderr, "cui_core: [probe] acquire idx=%u result=%d\n",
                     currentSwapchainImageIndex_, static_cast<int>(result));
        ++acquireLogs;
    }
    
    // Swapchain is out of date (window resized) — recreate and retry once
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        if (result == VK_ERROR_OUT_OF_DATE_KHR && xcbConnection_) {
            recreateSwapchain(width_, height_);
            // Retry acquire with the new swapchain
            vkWaitForFences(vh.device, 1, &vh.inFlightFence, VK_TRUE, UINT64_MAX);
            vkResetFences(vh.device, 1, &vh.inFlightFence);
            vkAcquireNextImageKHR = reinterpret_cast<PFN_vkAcquireNextImageKHR>(
                vkGetDeviceProcAddr(vh.device, "vkAcquireNextImageKHR"));
            result = vkAcquireNextImageKHR(vh.device, vh.swapchain, UINT64_MAX,
                                          vh.imageAvailableSemaphore, VK_NULL_HANDLE,
                                          &currentSwapchainImageIndex_);
        }
        // VK_SUBOPTIMAL_KHR is still usable
        if (result == VK_SUBOPTIMAL_KHR) return true;
    }
    
    return result == VK_SUCCESS;
}

bool SkiaRenderer::presentSwapchainImage() {
    if (!vulkanHandles_ || !vulkanHandles_->swapchainReady) return false;
    auto& vh = *vulkanHandles_;

    auto vkQueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(
        vkGetDeviceProcAddr(vh.device, "vkQueuePresentKHR"));

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &vh.renderFinishedSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &vh.swapchain;
    presentInfo.pImageIndices = &currentSwapchainImageIndex_;

    VkResult result = vkQueuePresentKHR(vh.queue, &presentInfo);
    static int presentLogs = 0;
    if (presentLogs < 10) {
        std::fprintf(stderr, "cui_core: [probe] present idx=%u result=%d\n",
                     currentSwapchainImageIndex_, static_cast<int>(result));
        ++presentLogs;
    }
    // VK_SUBOPTIMAL_KHR: present succeeded but image quality is suboptimal
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        return true;
    }
    // VK_ERROR_OUT_OF_DATE_KHR: swapchain no longer matches surface (resize) — will be recreated next frame
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return false;  // Frame will be retried with a new swapchain
    }
    std::fprintf(stderr, "cui_core: vkQueuePresentKHR failed: vkResult=%d\n", result);
    return false;
}

bool SkiaRenderer::beginFrame() {
#if CUI_SKIA_GRAPHITE_VULKAN
    ensureSwapchain();
    if (vulkanHandles_ && vulkanHandles_->swapchainReady)
        return acquireNextSwapchainImage();
#endif
    // 非 swapchain 模式（Graphite GPU surface / raster）：无需 acquire，
    // 恒返回 true 让 Application::renderFrame 继续绘制。
    return true;
}

void SkiaRenderer::endFrame() {
#if CUI_SKIA_GRAPHITE_VULKAN
    if (vulkanHandles_ && vulkanHandles_->swapchainReady) {
        // 以下 swapchain 呈现代码保留但默认不可达：initWithXcbWindow 已
        // 不再创建 swapchain（本机 WSI 呈现实测不更新屏幕，连 vkcube 亦然）。
        // 呈现统一由 Application::blitToNativeWindow 的 readPixels→
        // xcb_put_image 完成（dev-1 验证过的架构）。
        flush();
        presentSwapchainImage();
        if (xcbConnection_)
            xcb_flush(xcbConnection_);
    }
#endif
}

bool SkiaRenderer::createOffscreenTarget() {
#if CUI_SKIA_GRAPHITE_VULKAN
    if (!vulkanHandles_ || !vulkanHandles_->swapchainReady || !graphiteContext_) {
        return false;
    }
    
    // Create a new recorder if it was reset (e.g., during swapchain recreation)
    if (!graphiteRecorder_) {
        skgpu::graphite::RecorderOptions recOpts;
        recOpts.fImageProvider = sk_make_sp<CpuToGpuImageProvider>();
        graphiteRecorder_ = graphiteContext_->makeRecorder(recOpts);
        if (!graphiteRecorder_) {
            std::fprintf(stderr, "cui_core: Failed to recreate Graphite recorder\n");
            return false;
        }
    }

    // A freshly created buffer holds undefined pixels: the caller must
    // paint a full frame into it before presenting (上游 hands a
    // newly-created or resized surface a damage rect covering the WHOLE
    // surface for the same reason).
    destroyOffscreenTarget();

    SkImageInfo info = SkImageInfo::MakeN32Premul(width_, height_);

    // Let Graphite create its own render target texture
    offscreen_.surface = SkSurfaces::RenderTarget(graphiteRecorder_.get(), info);
    if (!offscreen_.surface) {
        std::fprintf(stderr, "cui_core: Failed to create the Graphite offscreen drawing buffer\n");
        return false;
    }

    // Extract the VkImage from Graphite's internal texture
    auto* graphiteSurface = static_cast<skgpu::graphite::Surface*>(offscreen_.surface.get());
    auto proxyView = graphiteSurface->target();
    if (!proxyView) {
        std::fprintf(stderr, "cui_core: Offscreen buffer has no target proxy\n");
        offscreen_.surface.reset();
        return false;
    }
    auto* proxy = proxyView.proxy();
    if (!proxy || !proxy->isInstantiated()) {
        std::fprintf(stderr, "cui_core: Offscreen buffer proxy not instantiated\n");
        offscreen_.surface.reset();
        return false;
    }
    auto* texture = proxy->texture();
    auto* vkTex = static_cast<skgpu::graphite::VulkanTexture*>(texture);
    offscreen_.vkImage = vkTex->vkImage();
    offscreen_.vkTexture = vkTex;

    if (offscreen_.vkImage == VK_NULL_HANDLE) {
        std::fprintf(stderr, "cui_core: Offscreen buffer VkImage is null\n");
        offscreen_.surface.reset();
        return false;
    }

    // 诊断探针：打印 Graphite 分配纹理的真实 usage 与当前 layout。
    // 我们的 blit 呈现路径假设 COLOR_ATTACHMENT_OPTIMAL + TRANSFER_SRC 可用，
    // 若实际不符则 vkCmdCopyImage 为非法用法（呈现空屏/花屏的根因候选）。
    {
        const auto& vkInfo = vkTex->vulkanTextureInfo();
        std::fprintf(stderr,
                     "cui_core: [probe] offscreen VkImage usage=0x%x layout=%d (COLOR_ATTACHMENT_OPTIMAL=%d)\n"
                     "cui_core: [probe] TRANSFER_SRC %s, TRANSFER_DST %s, SAMPLED %s\n",
                     vkInfo.fImageUsageFlags, static_cast<int>(vkTex->currentLayout()),
                     static_cast<int>(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
                     (vkInfo.fImageUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ? "yes" : "NO",
                     (vkInfo.fImageUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ? "yes" : "NO",
                     (vkInfo.fImageUsageFlags & VK_IMAGE_USAGE_SAMPLED_BIT) ? "yes" : "NO");
    }

    std::fprintf(stderr, "cui_core: Offscreen drawing buffer %dx%d (GPU→GPU copy to swapchain)\n", width_,
                 height_);
    return true;
#else
    return false;
#endif
}

void SkiaRenderer::destroyOffscreenTarget() {
#if CUI_SKIA_GRAPHITE_VULKAN
    // Graphite owns the VkImage — we just release our SkSurface reference
    offscreen_.surface.reset();
    offscreen_.vkImage = VK_NULL_HANDLE;
    offscreen_.vkTexture = nullptr;
#endif
}

void SkiaRenderer::destroySwapchainResources() {
#if CUI_SKIA_GRAPHITE_VULKAN
    if (!vulkanHandles_) return;
    auto& vh = *vulkanHandles_;
    
    if (vh.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(vh.device);
    }
    
    // Flush and submit any pending Graphite work before destroying resources
    if (graphiteContext_ && graphiteRecorder_) {
        auto recording = graphiteRecorder_->snap();
        if (recording) {
            skgpu::graphite::InsertRecordingInfo info;
            info.fRecording = recording.get();
            graphiteContext_->insertRecording(info);
        }
        if (graphiteContext_->hasPendingGPUWork())
            graphiteContext_->submit(skgpu::graphite::SyncToCpu::kYes);
    }
    
    // Destroy the offscreen buffer first (it references a Graphite texture)
    destroyOffscreenTarget();
    
    // Reset the recorder to clear any internal cached state referencing old textures.
    // A new recorder will be created when the offscreen buffer is recreated.
    if (graphiteContext_) {
        graphiteRecorder_.reset();
    }
    
    // Destroy blit command buffers (they were allocated from the command pool)
    vh.blitCommandBuffers.clear();
    
    // Destroy sync objects
    if (vh.imageAvailableSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(vh.device, vh.imageAvailableSemaphore, nullptr);
        vh.imageAvailableSemaphore = VK_NULL_HANDLE;
    }
    if (vh.renderFinishedSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(vh.device, vh.renderFinishedSemaphore, nullptr);
        vh.renderFinishedSemaphore = VK_NULL_HANDLE;
    }
    if (vh.inFlightFence != VK_NULL_HANDLE) {
        vkDestroyFence(vh.device, vh.inFlightFence, nullptr);
        vh.inFlightFence = VK_NULL_HANDLE;
    }
    
    // Command pool destruction also frees all command buffers allocated from it
    if (vh.commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(vh.device, vh.commandPool, nullptr);
        vh.commandPool = VK_NULL_HANDLE;
    }
    vh.commandBuffers.clear();
    
    // Destroy image views
    for (auto iv : vh.swapchainImageViews) {
        vkDestroyImageView(vh.device, iv, nullptr);
    }
    vh.swapchainImageViews.clear();
    
    // Destroy framebuffers
    for (auto fb : vh.swapchainFramebuffers) {
        vkDestroyFramebuffer(vh.device, fb, nullptr);
    }
    vh.swapchainFramebuffers.clear();
    
    // Destroy render pass
    if (vh.renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(vh.device, vh.renderPass, nullptr);
        vh.renderPass = VK_NULL_HANDLE;
    }
    
    // Destroy swapchain
    if (vh.swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(vh.device, vh.swapchain, nullptr);
        vh.swapchain = VK_NULL_HANDLE;
    }
    vh.swapchainImages.clear();
    
    // Destroy surface
    if (vh.surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(vh.instance, vh.surface, nullptr);
        vh.surface = VK_NULL_HANDLE;
    }
    
    vh.swapchainReady = false;
#endif
}

uint32_t SkiaRenderer::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const {
#if CUI_SKIA_GRAPHITE_VULKAN
    if (vulkanHandles_) {
        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(vulkanHandles_->physicalDevice, &memProps);
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props)
                return i;
        }
    }
#endif
    return 0;
}
#endif

#if CUI_SKIA_GRAPHITE_DAWN
bool SkiaRenderer::initDawn() {
    dawnHandles_ = std::make_unique<DawnHandles>();
    dawnHandles_->instance = wgpu::CreateInstance(nullptr);
    if (!dawnHandles_->instance) {
        std::fprintf(stderr, "cui_core: wgpu::CreateInstance failed\n");
        dawnHandles_.reset();
        return false;
    }

    wgpu::RequestAdapterOptions adapterOpts{};
    auto adapter = dawnHandles_->instance.RequestAdapterSync(&adapterOpts);
    if (!adapter) {
        std::fprintf(stderr, "cui_core: Dawn adapter request failed\n");
        dawnHandles_.reset();
        return false;
    }

    wgpu::RequestDeviceOptions deviceOpts{};
    dawnHandles_->device = adapter.RequestDeviceSync(&deviceOpts);
    if (!dawnHandles_->device) {
        std::fprintf(stderr, "cui_core: Dawn device request failed\n");
        dawnHandles_.reset();
        return false;
    }

    skgpu::graphite::DawnBackendContext dawnCtx;
    dawnCtx.fInstance = dawnHandles_->instance;
    dawnCtx.fDevice = dawnHandles_->device;
    dawnCtx.fQueue = dawnHandles_->device.GetQueue();
    skgpu::graphite::ContextOptions opts;
    graphiteContext_ = skgpu::graphite::ContextFactory::MakeDawn(dawnCtx, opts);
    if (graphiteContext_) {
        // 与 Vulkan 两处 makeRecorder 一致：必须挂上 CpuToGpuImageProvider。
        // 不挂就退回 Skia 内置的 DefaultImageProvider，它刻意不做任何转换，
        // 于是每张「Graphite 自己上传不了」的图（编码惰性图 / generator /
        // YUV）都会成对打出 "Couldn't convert SkImage to a Graphite-backed
        // representation" + "Key context creation failed … draw dropped"，
        // 图像区直接空白。
        skgpu::graphite::RecorderOptions recOpts;
        recOpts.fImageProvider = sk_make_sp<CpuToGpuImageProvider>();
        graphiteRecorder_ = graphiteContext_->makeRecorder(recOpts);
        std::fprintf(stderr, "cui_core: Graphite/Dawn context created\n");
        return true;
    }

    std::fprintf(stderr, "cui_core: Graphite/Dawn init failed, falling back\n");
    dawnHandles_.reset();
    return false;
}
#endif

#if CUI_SKIA_GRAPHITE_METAL
bool SkiaRenderer::initMetal() {
    // Metal context creation requires Objective-C runtime calls.
    // Placeholder — actual implementation needs MTLCreateSystemDefaultDevice()
    // and [device newCommandQueue] via ObjC message sends or bridging header.
    std::fprintf(stderr, "cui_core: Graphite/Metal not yet implemented, falling back\n");
    return false;
}
#endif

}  // namespace cui_core
