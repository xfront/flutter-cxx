#pragma once
/// @file foundation/image_data.h
/// @brief 编码字节 → 已解码图像的核心管线（pub.dev `image` 包的解码侧等价物）。
///
/// 本层只碰 Skia，不知道 Widget / RenderObject 的存在（cui_foundation 的边界）。
///
/// 三件事：
///   1. `decodeImage()` —— 用 Skia 的 `SkCodec` 从 PNG/JPEG/WebP/GIF/BMP/… 字节
///      解出 `DecodedImage`。顺手处理两件 `Image` widget 层容易漏掉的事：
///        - **EXIF 方向**：`SkCodec::getOrigin()` 的 8 种取值全部烘焙进像素，
///          于是上层永远只看到「正过来的图」。
///        - **多帧动画**：逐帧按 `FrameInfo::fRequiredFrame` / `fDisposalMethod`
///          合成（GIF 的局部帧只画增量，不合成就会闪）。
///   2. `scaleImage()` —— 解码期降采样（走 `SkCodec::getScaledDimensions`，比
///      解完整尺寸再缩便宜得多）与解码后的精确缩放。
///   3. 像素级几何运算 `cropImage` / `rotatedImage` / `flippedImage`，供编辑器
///      与「方向未烘焙」的兜底路径复用。
///
/// 约定：`DecodedImage` 用 `std::shared_ptr` 跨线程传递（`cui_core::SharedPtr` 禁止
/// 跨线程，见 foundation/shared_ptr.h 的铁律）。

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkRect.h>

namespace cui {

/// 动画的一帧。
struct ImageFrame {
    sk_sp<SkImage> image;   ///< 已与本帧之前的帧合成过的完整画面
    int durationMs = 0;     ///< 该帧展示时长（GIF 的 delay，0 表示静态）
};

/// 一次解码的结果。
struct DecodedImage {
    std::vector<ImageFrame> frames;
    int repetitionCount = 0;  ///< 0=静态，-1=无限循环（SkCodec 口径）
    bool animated = false;
    int encodedOrigin = 1;    ///< `SkEncodedOrigin`（1=kTopLeft）；已烘焙，仅记录用
    std::string format;       ///< "png"/"jpeg"/"gif"/…，未知时 "unknown"
    std::string error;        ///< 非空表示失败

    bool valid() const { return !frames.empty() && frames[0].image != nullptr; }
    int width() const { return valid() ? frames[0].image->width() : 0; }
    int height() const { return valid() ? frames[0].image->height() : 0; }

    /// 首帧（静态图的通用取法）。
    sk_sp<SkImage> image() const { return valid() ? frames[0].image : nullptr; }
    /// 动画总时长（毫秒），静态图为 0。
    int totalDurationMs() const;
    /// 失败原因（写日志用；成功时为空）。
    explicit operator bool() const { return valid(); }
};

using DecodedImagePtr = std::shared_ptr<DecodedImage>;

/// 解码选项。
struct DecodeOptions {
    /// 期望的解码宽度（像素）。给了且小于原图 ⇒ 让 codec 直接降采样解码。
    std::optional<int> targetWidth;
    std::optional<int> targetHeight;
    /// false ⇒ 动画只解首帧（省内存，静态展示常用）。
    bool decodeAllFrames = true;
};

/// 从编码字节解码。`data` 为空或不是可识别的编码格式 ⇒ 返回带 `error` 的结果。
DecodedImagePtr decodeImage(const sk_sp<SkData>& data, const DecodeOptions& options = {});
DecodedImagePtr decodeImage(const std::vector<uint8_t>& bytes, const DecodeOptions& options = {});

/// 读文件并解码（`SkData::MakeFromFileName`）。
DecodedImagePtr decodeImageFromFile(const std::string& path, const DecodeOptions& options = {});

/// 精确缩放到 w×h（保持像素，走高质量采样）。已经等尺寸时原样返回。
sk_sp<SkImage> scaleImage(const sk_sp<SkImage>& src, int w, int h);

/// 按 `SkCodec::getScaledDimensions` 的比例降采样整份解码结果（含全部动画帧）。
DecodedImagePtr rescaleDecoded(const DecodedImagePtr& src, std::optional<int> w,
                               std::optional<int> h);

/// 裁出矩形（像素坐标，自动夹到图像范围内）。
sk_sp<SkImage> cropImage(const sk_sp<SkImage>& src, const SkIRect& rect);

/// 旋转（`deg` 顺时针，任意角度都能转；90° 的整数倍是精确换边长，
/// 其余角度取旋转后的外接框并重采样）。
sk_sp<SkImage> rotatedImage(const sk_sp<SkImage>& src, int deg);

/// 水平/垂直翻转。
sk_sp<SkImage> flippedImage(const sk_sp<SkImage>& src, bool horizontal, bool vertical);

/// 把 `origin`（`SkEncodedOrigin`，1..8）表示的方向烘焙进像素。
/// 解码路径已自动处理，这个入口留给「外部塞进来的 SkImage + 已知的 EXIF 值」。
sk_sp<SkImage> applyOrientedImage(const sk_sp<SkImage>& src, int origin);

/// 编码为 PNG（保存/落盘用）。
sk_sp<SkData> encodePng(const sk_sp<SkImage>& image);

/// 图像字节占用（近似：像素数 × bytesPerPixel），内存缓存按它算预算。
size_t imageByteSize(const sk_sp<SkImage>& image);

}  // namespace cui_core
