/// @file test_image_core.cpp
/// @brief 图像核心层单元测试（解码 / 像素几何 / 三级缓存 / Provider 与加载器）。
///
/// 覆盖：
///   1. 解码：PNG 字节 → `DecodedImage`（尺寸 / format / origin）、坏数据报错、
///      `targetWidth` 降采样（codec 不支持降采样时补一次精确缩放）。
///   2. 几何：`cropImage`（越界夹取）、`rotatedImage`（90° 换边长）、`flippedImage`、
///      `applyOrientedImage`、`encodePng` 往返、`imageByteSize`、`rescaleDecoded`。
///   3. 动画：`examples/res/sample.gif` 多帧合成（时长、`decodeAllFrames=false` 只解首帧）。
///   4. 缓存：解码缓存按条目数淘汰 + 命中刷新 LRU；编码字节缓存按字节预算淘汰；
///      磁盘缓存读写 / TTL 过期 / prune / clear。
///   5. 加载器：同步快路径（已解码 provider / 解码缓存命中）、同 key 在飞去重、
///      取消语义、失败不入缓存、`cacheWidth` 参与缓存 key、进度合并。
///
/// 线程接缝在测试里全部装成「同步执行」，所以整条链路是确定性的。

#include <cstdio>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <include/core/SkCanvas.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkSurface.h>

#include "cui/foundation/image_cache.h"
#include "cui/foundation/image_data.h"
#include "cui/foundation/image_provider.h"

#ifndef CUI_SAMPLE_GIF
#define CUI_SAMPLE_GIF ""
#endif

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (ok) {
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

using cui::DecodedImagePtr;
using cui::ImageTask;

/// 生成一张纯色分块的 PNG（左半红、右半蓝），用于「像素真的被几何运算动过」的判定。
sk_sp<SkData> makePng(int w, int h) {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h));
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SK_ColorWHITE);
    SkPaint paint;
    paint.setColor(SK_ColorRED);
    canvas->drawRect(SkRect::MakeLTRB(0.0f, 0.0f, float(w / 2), float(h)), paint);
    paint.setColor(SK_ColorBLUE);
    canvas->drawRect(SkRect::MakeLTRB(float(w / 2), 0.0f, float(w), float(h)), paint);
    return cui::encodePng(surface->makeImageSnapshot());
}

std::vector<uint8_t> toBytes(const sk_sp<SkData>& data) {
    const uint8_t* p = static_cast<const uint8_t*>(data->data());
    return std::vector<uint8_t>(p, p + data->size());
}

/// 读一个像素（强制转成 RGBA8888，字节序与 N32/BGRA 平台无关）。
uint32_t pixelAt(const sk_sp<SkImage>& image, int x, int y) {
    const SkImageInfo info =
        SkImageInfo::Make(1, 1, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    uint32_t value = 0;
    if (!image->readPixels(info, &value, 4, x, y))
        return 0;
    return value;
}

bool isRedish(uint32_t c) {
    const uint8_t r = uint8_t(c & 0xFF), b = uint8_t((c >> 16) & 0xFF);
    return r > b;
}
bool isBlueish(uint32_t c) {
    const uint8_t r = uint8_t(c & 0xFF), b = uint8_t((c >> 16) & 0xFF);
    return b > r;
}

// ── 延迟执行的后台接缝 ──────────────────────────────────────────────────────

std::vector<ImageTask>& deferred() {
    static std::vector<ImageTask> tasks;
    return tasks;
}

void runDeferred() {
    std::vector<ImageTask> tasks;
    tasks.swap(deferred());
    for (ImageTask& t : tasks)
        t();
}

void installInlineSeams() {
    cui::setBackgroundExecutor([](ImageTask t) { t(); });
    cui::setUiDispatcher([](ImageTask t) { t(); });
}

void installDeferredSeams() {
    cui::setBackgroundExecutor([](ImageTask t) { deferred().push_back(std::move(t)); });
    cui::setUiDispatcher([](ImageTask t) { t(); });
}

void resetSeams() {
    cui::setBackgroundExecutor({});
    cui::setUiDispatcher({});
    cui::setHttpTransport({});
}

// ── 1. 解码 ─────────────────────────────────────────────────────────────────

void testDecode() {
    std::printf("== decode ==\n");
    const sk_sp<SkData> png = makePng(200, 100);
    check(bool(png) && png->size() > 0, "encodePng produces bytes");

    DecodedImagePtr still = cui::decodeImage(png);
    check(still->valid(), "decoded image is valid");
    check(still->error.empty(), "no error string");
    check(still->width() == 200 && still->height() == 100, "size 200x100");
    check(still->format == "png", "format reported as png");
    check(still->encodedOrigin == 1, "origin baked to kTopLeft");
    check(still->frames.size() == 1 && !still->animated, "single frame, not animated");
    check(still->image() != nullptr && still->totalDurationMs() == 0, "first frame accessor");

    const std::vector<uint8_t> junk = {0x00, 0x01, 0x02, 0x03, 0x04};
    DecodedImagePtr bad = cui::decodeImage(junk);
    check(!bad->valid(), "garbage bytes do not decode");
    check(!bad->error.empty(), "garbage bytes report an error");
    check(!bool(*bad), "explicit operator bool follows validity");

    cui::DecodeOptions scaled;
    scaled.targetWidth = 100;
    DecodedImagePtr half = cui::decodeImage(png, scaled);
    check(half->valid() && half->width() == 100 && half->height() == 50,
          "targetWidth halves both axes even when the codec cannot scale-decode");

    cui::DecodeOptions heightOnly;
    heightOnly.targetHeight = 50;
    DecodedImagePtr byHeight = cui::decodeImage(png, heightOnly);
    check(byHeight->valid() && byHeight->width() == 100 && byHeight->height() == 50,
          "targetHeight keeps the aspect ratio");

    check(!cui::decodeImage(sk_sp<SkData>())->valid(), "null data reports an error");
    check(cui::decodeImageFromFile("Z:/definitely/not/a/file.png")->error.size() > 0,
          "missing file reports an error");
}

// ── 2. 几何 ─────────────────────────────────────────────────────────────────

void testGeometry() {
    std::printf("== geometry ==\n");
    const DecodedImagePtr src = cui::decodeImage(makePng(200, 100));
    const sk_sp<SkImage> image = src->image();

    const sk_sp<SkImage> crop = cui::cropImage(image, SkIRect::MakeLTRB(50, 25, 150, 75));
    check(crop && crop->width() == 100 && crop->height() == 50, "crop gives the requested rect");
    check(cui::cropImage(image, SkIRect::MakeLTRB(150, 60, 999, 999))->width() == 50,
          "crop clamps an out-of-bounds rect");
    check(cui::cropImage(nullptr, SkIRect::MakeLTRB(0, 0, 10, 10)) == nullptr,
          "crop of null is null");

    const sk_sp<SkImage> rot = cui::rotatedImage(image, 90);
    check(rot && rot->width() == 100 && rot->height() == 200, "90 degree rotation swaps axes");
    check(cui::rotatedImage(image, 360)->width() == 200, "full turn keeps the size");
    check(cui::rotatedImage(image, -90)->width() == 100, "negative angle normalises");
    const sk_sp<SkImage> oblique = cui::rotatedImage(image, 45);
    check(oblique && oblique->width() >= 200 && oblique->height() >= 100,
          "arbitrary angle grows to the rotated bounding box");

    const sk_sp<SkImage> flip = cui::flippedImage(image, true, false);
    check(flip && flip->width() == 200 && flip->height() == 100, "flip keeps the size");
    check(isBlueish(pixelAt(flip, 10, 50)), "horizontal flip moves blue to the left edge");
    check(isRedish(pixelAt(image, 10, 50)), "original has red on the left edge");

    // origin 6 = 右上下（90° 旋转）；烘焙后边长互换
    const sk_sp<SkImage> oriented = cui::applyOrientedImage(image, 6);
    check(oriented && oriented->width() == 100 && oriented->height() == 200,
          "applyOrientedImage(6) swaps axes");
    check(cui::applyOrientedImage(image, 1) == image, "origin 1 is a no-op");

    const DecodedImagePtr rescaled = cui::rescaleDecoded(src, 40, std::nullopt);
    check(rescaled->width() == 40 && rescaled->height() == 20, "rescaleDecoded keeps the ratio");
    check(cui::rescaleDecoded(src, std::nullopt, std::nullopt) == src,
          "rescaleDecoded without a target returns the same object");

    check(cui::imageByteSize(image) == 200u * 100u * 4u, "imageByteSize counts bytes per pixel");
    const sk_sp<SkData> again = cui::encodePng(rot);
    check(cui::decodeImage(again)->width() == 100, "encode/decode round trip preserves size");
}

// ── 3. 动画 ─────────────────────────────────────────────────────────────────

void testAnimated() {
    std::printf("== animated ==\n");
    const std::string path = CUI_SAMPLE_GIF;
    if (path.empty()) {
        std::printf("  [SKIP] no sample gif path compiled in\n");
        return;
    }
    const DecodedImagePtr gif = cui::decodeImageFromFile(path);
    if (!gif->valid()) {
        std::printf("  [SKIP] sample gif unreadable: %s\n", gif->error.c_str());
        return;
    }
    check(gif->format == "gif", "format reported as gif");
    check(gif->frames.size() > 1, "multiple frames decoded");
    check(gif->animated, "animated flag set");
    check(gif->totalDurationMs() > 0, "frame durations are present");
    check(gif->frames[0].image->width() == gif->width(),
          "every frame is composited to the full canvas size");

    cui::DecodeOptions firstOnly;
    firstOnly.decodeAllFrames = false;
    const DecodedImagePtr still = cui::decodeImageFromFile(path, firstOnly);
    check(still->frames.size() == 1 && !still->animated, "decodeAllFrames=false gives one frame");
}

// ── 4. 内存缓存 ─────────────────────────────────────────────────────────────

void testMemoryCaches() {
    std::printf("== memory caches ==\n");
    cui::DecodedImageCache& decoded = cui::DecodedImageCache::instance();
    decoded.clear();
    const size_t savedMax = decoded.maxEntries();
    decoded.setMaxEntries(2);

    const DecodedImagePtr a = cui::decodeImage(makePng(20, 20));
    const DecodedImagePtr b = cui::decodeImage(makePng(30, 30));
    const DecodedImagePtr c = cui::decodeImage(makePng(40, 40));
    decoded.put("core:a", a);
    decoded.put("core:b", b);
    check(decoded.count() == 2, "two entries stored");
    check(decoded.get("core:a") != nullptr, "a is a hit");
    decoded.put("core:c", c);  // get(a) 把 a 提到队首 ⇒ 淘汰的是 b
    check(decoded.count() == 2, "insert evicts down to maxEntries");
    check(decoded.get("core:b") == nullptr, "least-recently-used entry was evicted");
    check(decoded.get("core:a") != nullptr && decoded.get("core:c") != nullptr,
          "recently used entries survive");
    decoded.remove("core:a");
    check(decoded.count() == 1, "remove drops one entry");
    decoded.setMaxEntries(0);
    decoded.put("core:d", a);
    check(decoded.count() == 0, "maxEntries=0 disables the cache");
    decoded.setMaxEntries(savedMax);
    decoded.clear();
    check(decoded.count() == 0, "clear empties it");

    cui::EncodedDataCache& encoded = cui::EncodedDataCache::instance();
    encoded.clear();
    const size_t savedBytes = encoded.maxBytes();
    const sk_sp<SkData> small = makePng(10, 10);
    const size_t one = small->size();
    encoded.setMaxBytes(one * 3);
    encoded.put("core:e1", small);
    encoded.put("core:e2", small);
    check(encoded.get("core:e1") != nullptr, "both blobs fit the budget");
    check(encoded.bytes() == one * 2, "byte accounting adds each blob");
    encoded.put("core:e3", small);
    encoded.put("core:e4", small);
    // get(e1) 已经把 e1 提到队首 ⇒ 超额时先淘汰的是 e2
    check(encoded.get("core:e2") == nullptr, "over-budget insert evicts the least-used blob");
    check(encoded.get("core:e1") != nullptr && encoded.get("core:e4") != nullptr,
          "recently used blobs survive");
    check(encoded.bytes() == one * 3, "byte accounting drops the evicted blob");
    const sk_sp<SkData> big = makePng(200, 100);
    if (big->size() > one * 3) {
        encoded.put("core:e5", big);
        check(encoded.get("core:e5") == nullptr, "an oversized blob is not stored");
    }
    encoded.setMaxBytes(one / 2);
    check(encoded.get("core:e4") == nullptr, "shrinking the budget evicts immediately");
    encoded.setMaxBytes(0);
    check(encoded.bytes() == 0, "maxBytes=0 empties the cache");
    encoded.setMaxBytes(savedBytes);
    encoded.clear();
}

// ── 5. 磁盘缓存 ─────────────────────────────────────────────────────────────

void testDiskCache() {
    std::printf("== disk cache ==\n");
    namespace fs = std::filesystem;
    const std::string dir =
        (fs::temp_directory_path() / "cui_image_core_test").string();
    std::error_code ec;
    fs::remove_all(dir, ec);
    cui::setDiskCacheDir(dir);
    check(cui::diskCacheDir() == dir, "host-set cache dir takes effect");
    check(cui::diskCacheDirExplicitlySet(), "explicitly-set flag");

    const std::string id = "net:https://example.com/core.png";
    check(!cui::hasDiskCache(id), "unknown id is not cached");
    const std::vector<uint8_t> payload = toBytes(makePng(200, 100));
    check(cui::writeDiskCache(id, payload.data(), payload.size()), "write succeeds");
    check(cui::hasDiskCache(id), "hasDiskCache sees the file");
    const std::vector<uint8_t> back = cui::readDiskCache(id);
    check(back.size() == payload.size(), "read returns the same byte count");
    check(std::memcmp(back.data(), payload.data(), back.size()) == 0, "bytes round trip intact");
    check(cui::diskCacheUsageBytes() >= payload.size(), "usage counts the file");
    check(cui::cacheIdFor("x").size() == 16 && cui::cacheIdFor("x") == cui::cacheIdFor("x"),
          "cacheIdFor is a stable 16-hex id");
    check(cui::cacheIdFor("x") != cui::cacheIdFor("y"), "different keys hash differently");

    const int savedAge = cui::diskCacheMaxAgeDays();
    cui::setDiskCacheMaxAgeDays(0);
    check(!cui::isDiskCacheExpired(id), "age<=0 means never expires");
    cui::setDiskCacheMaxAgeDays(1);
    check(!cui::isDiskCacheExpired(id), "fresh entry is not expired");
    // 把 mtime 拨回两天前 ⇒ 过期 ⇒ 读不到，prune 能删掉
    const fs::path file = fs::path(dir) / ("cui_img_" + cui::cacheIdFor(id));
    fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::hours(48), ec);
    check(cui::isDiskCacheExpired(id), "entry older than the TTL is expired");
    check(cui::readDiskCache(id).empty(), "expired entry is not read back");
    check(cui::pruneDiskCache() == 1, "pruneDiskCache removes exactly the expired file");
    check(!cui::hasDiskCache(id), "pruned entry is gone");
    cui::setDiskCacheMaxAgeDays(savedAge);

    check(cui::writeDiskCache("net:second", payload.data(), payload.size()), "second write ok");
    check(cui::clearDiskCache() >= payload.size(), "clearDiskCache reports the bytes freed");
    check(!cui::hasDiskCache("net:second"), "clearDiskCache removed it");
    check(cui::diskCacheUsageBytes() == 0, "usage back to zero after clear");
    cui::setDiskCacheDir("");
    fs::remove_all(dir, ec);
}

// ── 6. 加载器：内存 / 已解码 provider ───────────────────────────────────────

void testLoaderMemory() {
    std::printf("== loader: memory ==\n");
    installInlineSeams();
    cui::DecodedImageCache::instance().clear();

    const std::vector<uint8_t> bytes = toBytes(makePng(120, 60));
    auto provider = std::make_shared<cui::MemoryImageProvider>(bytes, "core-load-1");

    cui::ImageLoadResult got;
    int calls = 0;
    std::shared_ptr<cui::ImageRequest> request = cui::loadImage(
        provider, {}, [&](const cui::ImageLoadResult& r) { got = r; ++calls; });
    check(calls == 1, "inline executor delivers synchronously");
    check(got.ok() && got.image->width() == 120, "memory provider decodes");
    check(!got.fromCache && got.source == "memory", "first load is not a cache hit");
    check(got.bytes == int64_t(bytes.size()), "encoded byte count reported");
    check(request->done() && !request->cancelled(), "request marked done");
    check(cui::inflightImageRequests() == 0, "nothing left in flight");

    cui::ImageLoadResult again;
    cui::loadImage(provider, {}, [&](const cui::ImageLoadResult& r) { again = r; });
    check(again.fromCache, "second load hits the decoded cache");
    check(again.image == got.image, "same decoded object reused");

    // cacheWidth 参与 key ⇒ 缩略图和原图是两条互不顶掉的条目
    cui::ImageRequestOptions thumb;
    thumb.cacheWidth = 60;
    cui::ImageLoadResult small;
    cui::loadImage(provider, thumb, [&](const cui::ImageLoadResult& r) { small = r; });
    check(small.ok() && small.image->width() == 60, "cacheWidth resizes during decode");
    check(!small.fromCache, "different size is a different cache entry");
    check(cui::DecodedImageCache::instance().count() == 2, "both sizes cached");

    auto decodedProvider = std::make_shared<cui::DecodedImageProvider>(got.image, "core-decoded");
    cui::ImageLoadResult direct;
    cui::loadImage(decodedProvider, {}, [&](const cui::ImageLoadResult& r) { direct = r; });
    check(direct.ok() && direct.image == got.image, "decoded provider skips the round trip");
    check(!direct.fromCache && direct.source == "decoded", "decoded provider reports itself");
    check(cui::inflightImageRequests() == 0, "decoded provider never queues a job");

    cui::loadImage(nullptr, {}, [&](const cui::ImageLoadResult& r) {
        check(!r.ok() && !r.error.empty(), "null provider reports an error");
    });

    auto emptyMem = std::make_shared<cui::MemoryImageProvider>(std::vector<uint8_t>{}, "core-empty");
    cui::ImageLoadResult failed;
    cui::loadImage(emptyMem, {}, [&](const cui::ImageLoadResult& r) { failed = r; });
    check(!failed.ok() && !failed.error.empty(), "empty memory bytes fail with an error");
}

// ── 7. 加载器：去重与取消 ───────────────────────────────────────────────────

void testLoaderDedupeAndCancel() {
    std::printf("== loader: dedupe + cancel ==\n");
    cui::DecodedImageCache::instance().clear();
    installDeferredSeams();

    const std::vector<uint8_t> bytes = toBytes(makePng(90, 40));
    auto provider = std::make_shared<cui::MemoryImageProvider>(bytes, "core-dedupe");
    int delivered = 0;
    std::shared_ptr<cui::ImageRequest> r1 =
        cui::loadImage(provider, {}, [&](const cui::ImageLoadResult&) { ++delivered; });
    std::shared_ptr<cui::ImageRequest> r2 =
        cui::loadImage(provider, {}, [&](const cui::ImageLoadResult&) { ++delivered; });
    check(cui::inflightImageRequests() == 1, "same key shares one in-flight job");
    check(deferred().size() == 1, "the job was scheduled once");
    check(r1->cacheKey() == r2->cacheKey(), "waiters carry the same cache key");
    runDeferred();
    check(delivered == 2, "both waiters get the result");
    check(cui::inflightImageRequests() == 0, "group cleared after delivery");

    // 取消：全部等待者取消 ⇒ 分组取消 ⇒ 回调一个都不发
    delivered = 0;
    cui::DecodedImageCache::instance().clear();
    std::shared_ptr<cui::ImageRequest> r3 =
        cui::loadImage(provider, {}, [&](const cui::ImageLoadResult&) { ++delivered; });
    check(cui::inflightImageRequests() == 1, "second job queued after the cache was cleared");
    r3->cancel();
    check(r3->cancelled(), "request reports cancellation");
    runDeferred();
    check(delivered == 0, "cancelled waiter receives nothing");
    check(cui::inflightImageRequests() == 0, "cancelled group is cleared");

    // cancelAllImageRequests 不能在持锁时回调钩子（自死锁回归）
    std::shared_ptr<cui::ImageRequest> r4 =
        cui::loadImage(provider, {}, [&](const cui::ImageLoadResult&) { ++delivered; });
    check(r4 != nullptr, "request created before global cancel");
    cui::cancelAllImageRequests();
    check(r4->cancelled(), "global cancel marks the request");
    runDeferred();
    check(delivered == 0, "globally cancelled waiter receives nothing");

    installInlineSeams();
}

// ── 8. 网络 provider：假传输 + 落盘缓存 ─────────────────────────────────────

void testLoaderNetwork() {
    std::printf("== loader: network ==\n");
    namespace fs = std::filesystem;
    installInlineSeams();
    cui::DecodedImageCache::instance().clear();
    cui::EncodedDataCache::instance().clear();
    const std::string dir = (fs::temp_directory_path() / "cui_image_core_net").string();
    std::error_code ec;
    fs::remove_all(dir, ec);
    cui::setDiskCacheDir(dir);

    const std::vector<uint8_t> png = toBytes(makePng(160, 80));
    int transportCalls = 0;
    std::string lastUrl;
    cui::HttpGetFn fake = [&](const std::string& url, const cui::HttpHeaders& headers,
                              std::vector<uint8_t>& out,
                              const std::function<void(const cui::HttpProgress&)>& onProgress,
                              const std::shared_ptr<cui::ImageCancellation>&, std::string& error) {
        ++transportCalls;
        lastUrl = url;
        if (!headers.empty() && headers[0].first != "Authorization")
            error = "unexpected header";
        // 中间块（不足 32KB）应当被合并掉，只有最后一次全量进度会送达 UI。
        if (onProgress) {
            onProgress(cui::HttpProgress{int64_t(png.size() / 4), int64_t(png.size())});
            onProgress(cui::HttpProgress{int64_t(png.size()), int64_t(png.size())});
        }
        out = png;
        return true;
    };
    cui::setHttpTransport(fake);
    check(cui::hasHttpTransport(), "a transport is installed");

    const std::string url = "https://example.com/core-net.png";
    auto provider = std::make_shared<cui::NetworkImageProvider>(
        url, cui::HttpHeaders{{"Authorization", "Bearer x"}});
    check(provider->diskCacheable(), "network provider is disk-cacheable");
    check(provider->key() == "net:" + url, "network key is scheme-prefixed url");

    cui::ImageLoadResult got;
    int progressHits = 0;
    double lastFraction = -2;
    cui::loadImage(
        provider, {}, [&](const cui::ImageLoadResult& r) { got = r; },
        [&](const cui::HttpProgress& p) { ++progressHits; lastFraction = p.fraction(); });
    check(got.ok() && got.image->width() == 160, "network image decodes");
    check(transportCalls == 1 && lastUrl == url, "transport called once with the url");
    check(got.source == "network", "result reports the source");
    check(progressHits == 1, "sub-32KB progress chunks are coalesced");
    check(lastFraction == 1.0, "final progress reports a complete fraction");
    check(cui::hasDiskCache(provider->diskId()), "bytes landed in the disk cache");

    // 清掉两级内存缓存 ⇒ 只剩磁盘缓存 ⇒ 传输层不再被调用
    cui::DecodedImageCache::instance().clear();
    cui::EncodedDataCache::instance().clear();
    cui::ImageLoadResult fromDisk;
    cui::loadImage(provider, {}, [&](const cui::ImageLoadResult& r) { fromDisk = r; });
    check(fromDisk.ok(), "disk cache serves the second load");
    check(transportCalls == 1, "disk hit does not touch the transport");

    // 关掉磁盘缓存 ⇒ 重新走传输
    cui::ImageRequestOptions noCache;
    noCache.useDiskCache = false;
    noCache.useMemoryCache = false;
    cui::loadImage(provider, noCache, [](const cui::ImageLoadResult&) {});
    check(transportCalls == 2, "cache-off request goes back to the transport");

    // 失败：错误上抛，且不污染缓存
    cui::setHttpTransport(
        [&](const std::string&, const cui::HttpHeaders&, std::vector<uint8_t>&,
            const std::function<void(const cui::HttpProgress&)>&,
            const std::shared_ptr<cui::ImageCancellation>&, std::string& error) {
            error = "404 not found";
            return false;
        });
    auto missing = std::make_shared<cui::NetworkImageProvider>("https://example.com/missing.png");
    cui::ImageLoadResult bad;
    cui::loadImage(missing, {}, [&](const cui::ImageLoadResult& r) { bad = r; });
    check(!bad.ok() && bad.error == "404 not found", "transport failure surfaces the error");
    check(bad.image == nullptr, "failed load has no image");
    check(cui::DecodedImageCache::instance().get(cui::NetworkImageProvider("https://example.com/missing.png").key()) ==
              nullptr,
          "failure is not cached");

    // 坏字节：传输成功但不是图像
    cui::setHttpTransport(
        [&](const std::string&, const cui::HttpHeaders&, std::vector<uint8_t>& out,
            const std::function<void(const cui::HttpProgress&)>&,
            const std::shared_ptr<cui::ImageCancellation>&, std::string&) {
            out = std::vector<uint8_t>{1, 2, 3, 4};
            return true;
        });
    cui::ImageLoadResult undecodable;
    cui::loadImage(std::make_shared<cui::NetworkImageProvider>("https://example.com/junk.png"), {},
                   [&](const cui::ImageLoadResult& r) { undecodable = r; });
    check(!undecodable.ok() && !undecodable.error.empty(), "undecodable bytes fail");

    // 复位成内置默认：Windows 有 WinHTTP，其他平台没有后端时报错。
    cui::setHttpTransport({});
    cui::ImageLoadResult afterReset;
    cui::loadImage(std::make_shared<cui::NetworkImageProvider>("http://127.0.0.1:1/nope.png"), {},
                   [&](const cui::ImageLoadResult& r) { afterReset = r; });
    check(!afterReset.ok(), "reset transport cannot fetch a dead url");

    fs::remove_all(dir, ec);
    cui::setDiskCacheDir("");
}

// ── 9. 取消令牌 / 进度值类型 ────────────────────────────────────────────────

void testValueTypes() {
    std::printf("== value types ==\n");
    cui::HttpProgress unknown;
    check(unknown.fraction() == -1.0, "unknown total gives fraction -1");
    check(cui::HttpProgress{50, 100}.fraction() == 0.5, "known total gives a fraction");
    cui::ImageCancellation cancel;
    check(!cancel.cancelled(), "token starts live");
    cancel.cancel();
    check(cancel.cancelled(), "token is sticky");
}

}  // namespace

int main() {
    testDecode();
    testGeometry();
    testAnimated();
    testMemoryCaches();
    testDiskCache();
    testLoaderMemory();
    testLoaderDedupeAndCancel();
    testLoaderNetwork();
    testValueTypes();
    resetSeams();
    if (g_failures > 0) {
        std::printf("FAILED: %d checks\n", g_failures);
        return 1;
    }
    std::printf("all image core checks passed\n");
    return 0;
}
