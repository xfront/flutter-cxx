/// @file foundation/image_data.cpp — SkCodec 解码 + 像素级几何运算实现。
#include "cui/foundation/image_data.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <include/codec/SkCodec.h>
#include <include/codec/SkCodecAnimation.h>
#include <include/codec/SkEncodedImageFormat.h>
#include <include/codec/SkEncodedOrigin.h>
#include <include/core/SkAlphaType.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkSurface.h>
#include <include/core/SkTypes.h>
#include <include/encode/SkPngEncoder.h>

namespace cui {

namespace {

const char* formatName(SkEncodedImageFormat f) {
    switch (f) {
        case SkEncodedImageFormat::kBMP: return "bmp";
        case SkEncodedImageFormat::kGIF: return "gif";
        case SkEncodedImageFormat::kICO: return "ico";
        case SkEncodedImageFormat::kJPEG: return "jpeg";
        case SkEncodedImageFormat::kPNG: return "png";
        case SkEncodedImageFormat::kWBMP: return "wbmp";
        case SkEncodedImageFormat::kWEBP: return "webp";
        case SkEncodedImageFormat::kPKM: return "pkm";
        case SkEncodedImageFormat::kKTX: return "ktx";
        case SkEncodedImageFormat::kASTC: return "astc";
        case SkEncodedImageFormat::kDNG: return "dng";
        case SkEncodedImageFormat::kHEIF: return "heif";
        case SkEncodedImageFormat::kAVIF: return "avif";
        case SkEncodedImageFormat::kJPEGXL: return "jpegxl";
    }
    return "unknown";
}

/// 通用「按矩阵重画一张新图」。裁剪 / 旋转 / 翻转 / 方向烘焙全部走这里，
/// 好处是不用区分 raster / 惰性图像（`SkImage::makeSubset` 在惰性图像上会失败）。
sk_sp<SkImage> renderWithMatrix(const sk_sp<SkImage>& src, const SkMatrix& m, int w, int h) {
    if (!src || w <= 0 || h <= 0)
        return nullptr;
    if (m.isIdentity() && w == src->width() && h == src->height())
        return src;
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h));
    if (!surface)
        return nullptr;
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SK_ColorTRANSPARENT);
    canvas->concat(m);
    canvas->drawImage(src, 0.0f, 0.0f, SkSamplingOptions(SkFilterMode::kLinear), nullptr);
    return surface->makeImageSnapshot();
}

/// 把 src 的四个角映射到非负象限后需要的平移量。
SkMatrix normalizedMatrix(const SkMatrix& in, int srcW, int srcH, int* outW, int* outH) {
    SkRect bounds = in.mapRect(SkRect::MakeWH(float(srcW), float(srcH)));
    SkMatrix m = in;
    m.postTranslate(-bounds.x(), -bounds.y());
    if (outW)
        *outW = std::max(1, int(std::lround(bounds.width())));
    if (outH)
        *outH = std::max(1, int(std::lround(bounds.height())));
    return m;
}

}  // namespace

int DecodedImage::totalDurationMs() const {
    int total = 0;
    for (const ImageFrame& f : frames)
        total += f.durationMs;
    return total;
}

// ── 解码 ────────────────────────────────────────────────────────────────────

DecodedImagePtr decodeImage(const sk_sp<SkData>& data, const DecodeOptions& options) {
    auto out = std::make_shared<DecodedImage>();
    if (!data || data->size() == 0) {
        out->error = "empty encoded data";
        return out;
    }
    std::unique_ptr<SkCodec> codec = SkCodec::MakeFromData(data);
    if (!codec) {
        out->error = "unsupported image format";
        return out;
    }

    out->format = formatName(codec->getEncodedFormat());
    out->encodedOrigin = static_cast<int>(codec->getOrigin());
    const SkImageInfo native = codec->getInfo();
    const int frameCount = codec->getFrameCount();
    const bool animated = frameCount > 1 && options.decodeAllFrames;

    // 目标尺寸：保持比例的较小缩放比（上游 cacheWidth 的语义）。
    double ratio = 1.0;
    if (options.targetWidth && *options.targetWidth > 0)
        ratio = std::min(ratio, double(*options.targetWidth) / double(std::max(1, native.width())));
    if (options.targetHeight && *options.targetHeight > 0)
        ratio = std::min(ratio, double(*options.targetHeight) / double(std::max(1, native.height())));

    SkImageInfo info = native;
    if (ratio < 1.0) {
        if (animated) {
            // 动画逐帧合成必须按原始尺寸做（局部帧的坐标是原图坐标），
            // 所以先原尺寸解，最后整体缩放。
        } else {
            // 让 codec 自己挑一个能高效解出的近似尺寸（JPEG 的 DCT 缩放）。
            SkISize scaled = codec->getScaledDimensions(float(ratio));
            if (scaled.width() > 0 && scaled.height() > 0)
                info = native.makeWH(scaled.width(), scaled.height());
        }
    }

    if (!animated) {
        auto [image, result] = codec->getImage(info, nullptr);
        if (!image) {
            out->error = result == SkCodec::Result::kIncompleteInput ? "incomplete image data"
                                                                     : "decode failed";
            return out;
        }
        // codec 未必支持降采样（PNG / 无 DCT 缩放），这时补一次精确缩放，
        // 保证上层拿到的尺寸就是 cacheWidth/cacheHeight 要的那个数。
        if (ratio < 1.0) {
            const int wantW = std::max(1, int(std::lround(native.width() * ratio)));
            const int wantH = std::max(1, int(std::lround(native.height() * ratio)));
            if (image->width() != wantW || image->height() != wantH)
                image = scaleImage(image, wantW, wantH);
        }
        out->frames.push_back(ImageFrame{std::move(image), 0});
    } else {
        std::vector<SkCodec::FrameInfo> infos = codec->getFrameInfo();
        if (int(infos.size()) < frameCount)
            infos.resize(size_t(frameCount));

        // 逐帧合成：dst 缓冲始终保存「上一帧合成完的画面」，把它的帧号告诉
        // codec，codec 负责按 fRequiredFrame / fDisposalMethod 补 blend 与清理。
        // 上一帧的处置方式是 kRestorePrevious 时它不能当下一个的底，只能退回
        // kNoFrame 让 codec 重解所需帧。
        const size_t rowBytes = size_t(info.minRowBytes());
        std::vector<uint8_t> buffer(rowBytes * size_t(info.height()));
        int prior = SkCodec::kNoFrame;
        for (int i = 0; i < frameCount; ++i) {
            SkCodec::Options opts;
            opts.fFrameIndex = i;
            opts.fPriorFrame = prior;
            const SkCodec::Result r = codec->getPixels(info, buffer.data(), rowBytes, &opts);
            if (r != SkCodec::Result::kSuccess && r != SkCodec::Result::kIncompleteInput)
                break;
            const SkPixmap pm(info, buffer.data(), rowBytes);
            sk_sp<SkImage> frame = SkImages::RasterFromPixmapCopy(pm);
            if (!frame)
                break;
            ImageFrame f;
            f.image = std::move(frame);
            f.durationMs = i < int(infos.size()) ? infos[size_t(i)].fDuration : 0;
            out->frames.push_back(std::move(f));
            const bool restorePrev =
                i < int(infos.size()) &&
                infos[size_t(i)].fDisposalMethod == SkCodecAnimation::DisposalMethod::kRestorePrevious;
            prior = restorePrev ? SkCodec::kNoFrame : i;
        }
        if (out->frames.empty()) {
            out->error = "decode failed";
            return out;
        }
        out->repetitionCount = codec->getRepetitionCount();
        // 动画走的是 getPixels，不含 EXIF 方向烘焙 ⇒ 这里补一次。
        if (out->encodedOrigin > 1) {
            for (ImageFrame& f : out->frames)
                f.image = applyOrientedImage(f.image, out->encodedOrigin);
        }
        if (ratio < 1.0) {
            int tw = std::max(1, int(std::lround(native.width() * ratio)));
            int th = std::max(1, int(std::lround(native.height() * ratio)));
            for (ImageFrame& f : out->frames)
                f.image = scaleImage(f.image, tw, th);
        }
    }

    out->animated = out->frames.size() > 1;
    return out;
}

DecodedImagePtr decodeImage(const std::vector<uint8_t>& bytes, const DecodeOptions& options) {
    return decodeImage(SkData::MakeWithCopy(bytes.data(), bytes.size()), options);
}

DecodedImagePtr decodeImageFromFile(const std::string& path, const DecodeOptions& options) {
    sk_sp<SkData> data = SkData::MakeFromFileName(path.c_str());
    if (!data) {
        auto out = std::make_shared<DecodedImage>();
        out->error = "cannot read file: " + path;
        return out;
    }
    return decodeImage(std::move(data), options);
}

// ── 缩放 ────────────────────────────────────────────────────────────────────

sk_sp<SkImage> scaleImage(const sk_sp<SkImage>& src, int w, int h) {
    if (!src || w <= 0 || h <= 0)
        return src;
    if (src->width() == w && src->height() == h)
        return src;
    sk_sp<SkImage> scaled = src->makeScaled(SkImageInfo::MakeN32Premul(w, h),
                                            SkSamplingOptions(SkCubicResampler::Mitchell()));
    if (scaled)
        return scaled;
    const float sx = float(w) / float(src->width());
    const float sy = float(h) / float(src->height());
    return renderWithMatrix(src, SkMatrix::MakeAll(sx, 0, 0, 0, sy, 0, 0, 0, 1), w, h);
}

DecodedImagePtr rescaleDecoded(const DecodedImagePtr& src, std::optional<int> w,
                               std::optional<int> h) {
    if (!src || !src->valid() || (!w && !h))
        return src;
    const int sw = src->width();
    const int sh = src->height();
    double ratio = 1.0;
    if (w && *w > 0)
        ratio = std::min(ratio, double(*w) / double(std::max(1, sw)));
    if (h && *h > 0)
        ratio = std::min(ratio, double(*h) / double(std::max(1, sh)));
    if (ratio >= 1.0)
        return src;
    const int tw = std::max(1, int(std::lround(sw * ratio)));
    const int th = std::max(1, int(std::lround(sh * ratio)));
    auto out = std::make_shared<DecodedImage>(*src);
    out->frames.clear();
    for (const ImageFrame& f : src->frames) {
        ImageFrame n = f;
        n.image = scaleImage(f.image, tw, th);
        out->frames.push_back(std::move(n));
    }
    return out;
}

// ── 几何运算 ────────────────────────────────────────────────────────────────

sk_sp<SkImage> cropImage(const sk_sp<SkImage>& src, const SkIRect& rect) {
    if (!src)
        return nullptr;
    const SkIRect bounds = SkIRect::MakeWH(src->width(), src->height());
    SkIRect r = rect;
    if (!r.intersect(bounds))
        return nullptr;
    return renderWithMatrix(src, SkMatrix::MakeAll(1, 0, -float(r.left()), 0, 1, -float(r.top()), 0, 0, 1),
                            r.width(), r.height());
}

sk_sp<SkImage> rotatedImage(const sk_sp<SkImage>& src, int deg) {
    if (!src)
        return nullptr;
    int d = deg % 360;
    if (d < 0)
        d += 360;
    if (d == 0)
        return src;
    int w = 0, h = 0;
    SkMatrix m = normalizedMatrix(SkMatrix::RotateDeg(float(d)), src->width(), src->height(), &w, &h);
    return renderWithMatrix(src, m, w, h);
}

sk_sp<SkImage> flippedImage(const sk_sp<SkImage>& src, bool horizontal, bool vertical) {
    if (!src || (!horizontal && !vertical))
        return src;
    const float sx = horizontal ? -1.0f : 1.0f;
    const float sy = vertical ? -1.0f : 1.0f;
    int w = 0, h = 0;
    SkMatrix m =
        normalizedMatrix(SkMatrix::MakeAll(sx, 0, 0, 0, sy, 0, 0, 0, 1), src->width(), src->height(), &w, &h);
    return renderWithMatrix(src, m, w, h);
}

sk_sp<SkImage> applyOrientedImage(const sk_sp<SkImage>& src, int origin) {
    if (!src || origin <= 1 || origin > int(SkEncodedOrigin::kLast_SkEncodedOrigin))
        return src;
    const auto o = static_cast<SkEncodedOrigin>(origin);
    const SkMatrix m = SkEncodedOriginToMatrix(o, src->width(), src->height());
    const bool swap = SkEncodedOriginSwapsWidthHeight(o);
    const int w = swap ? src->height() : src->width();
    const int h = swap ? src->width() : src->height();
    return renderWithMatrix(src, m, w, h);
}

// ── 编码 / 尺寸 ─────────────────────────────────────────────────────────────

sk_sp<SkData> encodePng(const sk_sp<SkImage>& image) {
    if (!image)
        return nullptr;
    return SkPngEncoder::Encode(nullptr, image.get(), SkPngEncoder::Options{});
}

size_t imageByteSize(const sk_sp<SkImage>& image) {
    if (!image)
        return 0;
    return size_t(image->width()) * size_t(image->height()) * 4u;
}

}  // namespace cui_core
