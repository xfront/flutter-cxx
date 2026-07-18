#pragma once
/// @file video_demo.h — gallery 视频页的运行时素材。
///
/// 仓库里不放二进制视频。页面打开时用 Skia 画 40 帧（480×270、10 fps、4 秒），
/// 逐帧 JPEG 编码后再手写一个最小 RIFF/'AVI ' 容器（MJPEG 码流 + 一条 PCM 音轨
/// + idx1 索引），交给 Media Foundation / FFmpeg 原生后端去解。这样两个后端
/// 吃到的都是真容器（**带音轨的**），演示与冒烟不依赖任何外部素材。
///
/// 音轨是正弦音阶 + 整点轻击（`buildDemoAudio`）：听得出「现在播到第几秒」，
/// 与画面左上那四个方块的亮块同一套记号。不要音轨时传一个空 `PcmTrack`，
/// 容器就退回「只有一条视频流」，字节与从前一致。
///
/// 帧画面是纯几何图形：
///   - 左上 4 个方块的亮块 = 第几秒（0..3），拖动进度条时整片底色跟着换；
///   - 右上 6 个小圆点是帧号的二进制（低位在右）；
///   - 中间摆锤的相位 = 进度，底部蓝条 = 播放进度，小球 = 匀速横移。
///   这些记号让截图能直接判断「seek 到哪一帧」。
///
/// CUI_VIDEO=<path> 时 gallery 页面改用用户给的视频文件，本文件不参与。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkRRect.h>
#include <include/core/SkSurface.h>
#include <include/encode/SkJpegEncoder.h>

namespace cui {
namespace story {
namespace video_demo {

// ── RIFF 写入器 ─────────────────────────────────────────────────────────────

/// 小端字节流。容器里所有整数都是 little-endian。
class ByteWriter {
public:
    std::vector<uint8_t> bytes;

    void u8(uint8_t v) { bytes.push_back(v); }
    void u16(uint16_t v) {
        u8(static_cast<uint8_t>(v & 0xFF));
        u8(static_cast<uint8_t>((v >> 8) & 0xFF));
    }
    void u32(uint32_t v) {
        u8(static_cast<uint8_t>(v & 0xFF));
        u8(static_cast<uint8_t>((v >> 8) & 0xFF));
        u8(static_cast<uint8_t>((v >> 16) & 0xFF));
        u8(static_cast<uint8_t>((v >> 24) & 0xFF));
    }
    void tag(const char* t) { bytes.insert(bytes.end(), t, t + 4); }
    void raw(const uint8_t* p, size_t n) { bytes.insert(bytes.end(), p, p + n); }
    size_t size() const { return bytes.size(); }

    void patchU32(size_t at, uint32_t v) {
        bytes[at] = static_cast<uint8_t>(v & 0xFF);
        bytes[at + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        bytes[at + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        bytes[at + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    }
};

/// 开一个 LIST：'LIST' + size + type。返回 size 字段的偏移。
inline size_t beginList(ByteWriter& w, const char* type) {
    w.tag("LIST");
    const size_t sizePos = w.size();
    w.u32(0);
    w.tag(type);
    return sizePos;
}

/// LIST 的 size 从 type 起算（含 4 字节 type），到写入点为止。
inline void endList(ByteWriter& w, size_t sizePos) {
    w.patchU32(sizePos, static_cast<uint32_t>(w.size() - (sizePos + 4)));
}

/// 普通块：fourcc + size + data（奇数长度补 0，RIFF 对齐要求）。
inline void chunk(ByteWriter& w, const char* id, const uint8_t* data, size_t n) {
    w.tag(id);
    w.u32(static_cast<uint32_t>(n));
    w.raw(data, n);
    if (n & 1)
        w.u8(0);
}

// ── MJPEG-in-AVI ────────────────────────────────────────────────────────────

/// 随容器一起交的一条 PCM 音轨（16bit 定点、交织）。
/// 空（`samples` 为空）= 不带音轨，容器里就只有一条视频流。
struct PcmTrack {
    std::vector<int16_t> samples;  ///< 交织采样：L R L R …
    int sampleRate = 0;
    int channels = 1;

    bool valid() const {
        return !samples.empty() && sampleRate > 0 && channels > 0 &&
               samples.size() % size_t(channels) == 0;
    }
    /// 采样帧数（每帧 `channels` 个样本）。
    size_t frames() const { return valid() ? samples.size() / size_t(channels) : 0; }
};

/// 演示音轨：`seconds` 秒里每段一个音符（A4/C5/E5/A5，每个占 1 秒），段首一记
/// 轻击。用途只是「听得出现在播到第几秒」 —— 与画面左上那四个亮块同一套记号，
/// 也刚好把「音频解码 → AudioRing → 设备」整条链路跑成真声音。
inline PcmTrack buildDemoAudio(int sampleRate, int channels, int seconds) {
    PcmTrack track;
    track.sampleRate = sampleRate > 0 ? sampleRate : 44100;
    track.channels = channels > 0 ? channels : 1;
    if (seconds <= 0)
        return track;
    // 四个音符循环用到超长素材上； frequencies 按段号取。
    static constexpr float kNotes[4] = {440.0f, 523.25f, 659.25f, 880.0f};
    const size_t frames = size_t(track.sampleRate) * size_t(seconds);
    track.samples.assign(frames * size_t(track.channels), 0);
    const double frameSeconds = 1.0 / double(track.sampleRate);
    for (size_t i = 0; i < frames; ++i) {
        const int segment = int(double(i) / double(track.sampleRate));
        // `inSegment` 是段内的**帧数**，包络一律换成秒：直接把帧数当秒用会让
        // 指数衰减快 4 万倍，整条音轨在第三帧之后就是静音（听起来像没声）。
        const double inSegment = double(i) - double(segment) * double(track.sampleRate);
        const double t = inSegment * frameSeconds;
        const float freq = kNotes[size_t(segment) % 4];
        // 包络：30ms 起音 + 指数衰减到下一段（听起来像拨弦，静音处不会爆音）。
        const double attack = t < 0.030 ? t / 0.030 : 1.0;
        const double decay = t < 0.030 ? 1.0 : std::exp(-(t - 0.030) * 2.6);
        double value = attack * decay * std::sin(6.283185307179586 * freq * double(i) * frameSeconds);
        // 整点轻击：一段开头 4ms 的高频噪声，给「换秒」一个明确的听感边界。
        if (t < 0.004)
            value += (0.004 - t) / 0.004 * 0.35 *
                     std::sin(6.283185307179586 * 2200.0 * double(i) * frameSeconds);
        const auto clamped = int32_t(std::lround(std::clamp(value, -1.0, 1.0) * 24000.0));
        for (int c = 0; c < track.channels; ++c)
            track.samples[i * size_t(track.channels) + size_t(c)] = int16_t(clamped);
    }
    return track;
}

/// 把逐帧 JPEG 封成 AVI 1.0（对齐 ffmpeg 的写法：idx1 偏移相对 'movi' fourcc）。
/// 带 `audio` 时多一条 'auds' 流（PCM16，块 id '01wb'），与视频帧交错写入 ——
/// 交错比「整段音轨放末尾」更接近真实 muxer 的产物，也免了 seek 时音频落在
/// 索引之外。尾置默认参 ⇒ 不要音轨的老调用点行为逐字节不变。
inline std::vector<uint8_t> buildMjpegAvi(const std::vector<std::vector<uint8_t>>& frames,
                                          int width, int height, int fps,
                                          const PcmTrack& audio = {}) {
    ByteWriter w;
    const uint32_t frameCount = static_cast<uint32_t>(frames.size());
    uint32_t maxFrame = 0;
    for (const auto& f : frames)
        maxFrame = std::max(maxFrame, static_cast<uint32_t>(f.size()));
    const uint32_t rate = fps > 0 ? static_cast<uint32_t>(fps) : 1u;
    const bool withAudio = audio.valid() && frameCount > 0;
    const size_t audioFrames = withAudio ? audio.frames() : 0;
    const uint32_t blockAlign = withAudio ? uint32_t(audio.channels) * 2u : 0u;
    uint32_t maxAudioChunk = 0;
    if (withAudio) {
        // 一段视频帧配多少采样帧：按整条音轨均分到各帧（末帧拿余数）。
        const size_t sliceEnd = audioFrames - audioFrames * size_t(frameCount - 1) / size_t(frameCount);
        maxAudioChunk = uint32_t(sliceEnd * size_t(blockAlign));
    }

    w.tag("RIFF");
    const size_t riffSizePos = w.size();
    w.u32(0);
    w.tag("AVI ");

    // ── hdrl ────────────────────────────────────────────────────────────────
    const size_t hdrlPos = beginList(w, "hdrl");

    // MainAVIHeader（56 字节）
    w.tag("avih");
    w.u32(56);
    w.u32(1000000u / rate);      // dwMicroSecPerFrame
    w.u32(maxFrame * rate);      // dwMaxBytesPerSec（估算）
    w.u32(0);                    // dwPaddingGranularity
    w.u32(0x00000010u);          // dwFlags = AVIF_HASINDEX
    w.u32(frameCount);           // dwTotalFrames
    w.u32(0);                    // dwInitialFrames
    w.u32(withAudio ? 2u : 1u);  // dwStreams
    w.u32(std::max(maxFrame, maxAudioChunk));  // dwSuggestedBufferSize
    w.u32(static_cast<uint32_t>(width));
    w.u32(static_cast<uint32_t>(height));
    w.u32(0); w.u32(0); w.u32(0); w.u32(0);  // dwReserved[4]

    // ── strl ────────────────────────────────────────────────────────────────
    const size_t strlPos = beginList(w, "strl");

    // AVIStreamHeader（56 字节）
    w.tag("strh");
    w.u32(56);
    w.tag("vids");
    w.tag("MJPG");
    w.u32(0);                    // dwFlags
    w.u16(0); w.u16(0);          // wPriority / wLanguage
    w.u32(0);                    // dwInitialFrames
    w.u32(1);                    // dwScale
    w.u32(rate);                 // dwRate（帧率 = rate/scale）
    w.u32(0);                    // dwStart
    w.u32(frameCount);           // dwLength
    w.u32(maxFrame);             // dwSuggestedBufferSize
    w.u32(0xFFFFFFFFu);          // dwQuality（-1 = 默认）
    w.u32(0);                    // dwSampleSize（视频恒 0）
    w.u16(0); w.u16(0);          // rcFrame.left / top
    w.u16(static_cast<uint16_t>(width));
    w.u16(static_cast<uint16_t>(height));

    // strf = BITMAPINFOHEADER（40 字节），biCompression = 'MJPG'
    w.tag("strf");
    w.u32(40);
    w.u32(40);                   // biSize
    w.u32(static_cast<uint32_t>(width));
    w.u32(static_cast<uint32_t>(height));
    w.u16(1);                    // biPlanes
    w.u16(24);                   // biBitCount
    w.tag("MJPG");               // biCompression
    w.u32(static_cast<uint32_t>(width) * static_cast<uint32_t>(height) * 3u);
    w.u32(0); w.u32(0);          // 分辨率（像素/米）
    w.u32(0); w.u32(0);          // biClrUsed / biClrImportant

    endList(w, strlPos);

    // ── 第二条 strl：'auds' + WAVEFORMATEX ─────────────────────────────────
    if (withAudio) {
        const size_t audStrlPos = beginList(w, "strl");
        w.tag("strh");
        w.u32(56);
        w.tag("auds");
        w.u32(1);                    // fccHandler = WAVE_FORMAT_PCM（四个字节存数字）
        w.u32(0);                    // dwFlags
        w.u16(0); w.u16(0);          // wPriority / wLanguage
        w.u32(0);                    // dwInitialFrames
        w.u32(1);                    // dwScale
        w.u32(uint32_t(audio.sampleRate));  // dwRate ⇒ 采样率 = rate/scale
        w.u32(0);                    // dwStart
        w.u32(uint32_t(audioFrames));       // dwLength = 采样帧数
        w.u32(uint32_t(audio.sampleRate) * blockAlign);  // dwSuggestedBufferSize
        w.u32(0xFFFFFFFFu);          // dwQuality
        w.u32(blockAlign);           // dwSampleSize = 块对齐（一帧的字节数）
        w.u16(0); w.u16(0); w.u16(0); w.u16(0);  // rcFrame（音频全 0）

        // strf = WAVEFORMATEX（18 字节，cbSize=0）
        w.tag("strf");
        w.u32(18);
        w.u16(1);                    // wFormatTag = PCM
        w.u16(uint16_t(audio.channels));
        w.u32(uint32_t(audio.sampleRate));
        w.u32(uint32_t(audio.sampleRate) * blockAlign);  // avgBytesPerSec
        w.u16(uint16_t(blockAlign));
        w.u16(16);                   // wBitsPerSample
        w.u16(0);                    // cbSize
        endList(w, audStrlPos);
    }

    endList(w, hdrlPos);

    // ── movi ────────────────────────────────────────────────────────────────
    const size_t moviPos = beginList(w, "movi");
    const size_t moviTagPos = moviPos + 4;  // 'movi' fourcc 的位置
    struct IndexEntry {
        const char* fourcc;
        uint32_t flags;
        uint32_t offset;
        uint32_t size;
    };
    std::vector<IndexEntry> index;
    index.reserve(frames.size() * (withAudio ? 2u : 1u));
    size_t audioCursor = 0;
    for (size_t i = 0; i < frames.size(); ++i) {
        // idx1 口径：块头相对 'movi' fourcc（第一块 = 4），与 ffmpeg 一致。
        index.push_back({"00dc", 0x00000010u, uint32_t(w.size() - moviTagPos),
                         uint32_t(frames[i].size())});
        chunk(w, "00dc", frames[i].data(), frames[i].size());
        if (!withAudio)
            continue;
        // 这一帧配多少采样帧：把整条音轨按帧序均分（末帧拿余数），保证
        // 逐帧的音频时长与视频帧时长对得上。
        const size_t sliceEnd = i + 1 < frames.size()
                                        ? audioFrames * (i + 1) / frames.size()
                                        : audioFrames;
        const size_t sliceFrames = sliceEnd - audioCursor;
        const uint8_t* pcm = reinterpret_cast<const uint8_t*>(
                audio.samples.data() + audioCursor * size_t(audio.channels));
        const size_t bytes = sliceFrames * size_t(blockAlign);
        index.push_back({"01wb", 0x00000010u, uint32_t(w.size() - moviTagPos),
                         uint32_t(bytes)});
        chunk(w, "01wb", pcm, bytes);
        audioCursor = sliceEnd;
    }
    endList(w, moviPos);

    // ── idx1 ────────────────────────────────────────────────────────────────
    w.tag("idx1");
    w.u32(uint32_t(index.size() * 16));
    for (const auto& e : index) {
        w.tag(e.fourcc);
        w.u32(e.flags);
        w.u32(e.offset);
        w.u32(e.size);
    }

    w.patchU32(riffSizePos, static_cast<uint32_t>(w.size() - (riffSizePos + 4)));
    return std::move(w.bytes);
}

// ── 画面 ────────────────────────────────────────────────────────────────────

inline sk_sp<SkImage> renderDemoFrame(int index, int count, int width, int height) {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    if (!surface)
        return nullptr;
    SkCanvas* canvas = surface->getCanvas();

    const double t = count > 1 ? double(index) / double(count - 1) : 0.0;
    const double phase = t * 6.283185307179586;

    // 底色随进度在三处色相之间缓慢漂移（四秒里看得出「换段」）。
    const uint8_t baseR = static_cast<uint8_t>(24 + 60 * (0.5 + 0.5 * std::sin(phase * 2.0)));
    const uint8_t baseG = static_cast<uint8_t>(44 + 70 * (0.5 + 0.5 * std::sin(phase * 2.0 + 2.09)));
    const uint8_t baseB = static_cast<uint8_t>(78 + 90 * (0.5 + 0.5 * std::sin(phase * 2.0 + 4.19)));
    canvas->clear(SkColorSetRGB(baseR, baseG, baseB));

    SkPaint p;
    p.setAntiAlias(true);

    // 淡网格：给运动一个参照系。
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(1.0f);
    p.setColor(SkColorSetARGB(26, 255, 255, 255));
    for (int x = 0; x < width; x += 40)
        canvas->drawLine(SkScalar(x), 0.0f, SkScalar(x), SkScalar(height), p);
    for (int y = 0; y < height; y += 40)
        canvas->drawLine(0.0f, SkScalar(y), SkScalar(width), SkScalar(y), p);

    // 摆锤：相位 = 进度（转两圈），中心圆 + 摆臂。
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(5.0f);
    p.setStrokeCap(SkPaint::kRound_Cap);
    p.setColor(SkColorSetARGB(150, 255, 255, 255));
    const float cx = float(width) * 0.5f;
    const float cy = float(height) * 0.5f;
    const float angle = float(phase * 2.0) - 1.5707963f;
    canvas->drawLine(cx, cy, cx + std::cos(angle) * 54.0f, cy + std::sin(angle) * 54.0f, p);
    p.setStrokeWidth(2.0f);
    p.setColor(SkColorSetARGB(110, 255, 255, 255));
    canvas->drawCircle(cx, cy, 62.0f, p);

    // 小球 + 拖尾：横向匀速，纵向正弦。
    p.setStyle(SkPaint::kFill_Style);
    const float margin = 46.0f;
    const float ballX = margin + (float(width) - 2.0f * margin) * float(t);
    const float ballY = float(height) * 0.46f + std::sin(phase * 3.0) * float(height) * 0.18f;
    for (int k = 5; k >= 1; --k) {
        p.setColor(SkColorSetARGB(static_cast<uint8_t>(22 * (6 - k)), 255, 220, 120));
        canvas->drawCircle(ballX - float(k) * 14.0f, ballY, 20.0f - 2.0f * float(k), p);
    }
    p.setColor(SkColorSetARGB(255, 255, 226, 130));
    canvas->drawCircle(ballX, ballY, 22.0f, p);
    p.setColor(SkColorSetARGB(255, 255, 255, 255));
    canvas->drawCircle(ballX - 7.0f, ballY - 7.0f, 6.0f, p);

    // 左上：四个方块的亮块 = 第几秒（0..3）。
    const int secondIndex = std::min(3, int(t * 4.0));
    for (int q = 0; q < 4; ++q) {
        p.setColor(q == secondIndex ? SkColorSetARGB(255, 255, 210, 90)
                                    : SkColorSetARGB(70, 255, 255, 255));
        canvas->drawRect(SkRect::MakeXYWH(24.0f + float(q) * 26.0f, 20.0f, 18.0f, 10.0f), p);
    }

    // 右上：6 个圆点 = 帧号的二进制（低位在右）。
    for (int bit = 0; bit < 6; ++bit) {
        const bool lit = ((index >> bit) & 1) != 0;
        p.setColor(lit ? SkColorSetARGB(255, 255, 255, 255)
                       : SkColorSetARGB(60, 255, 255, 255));
        canvas->drawCircle(float(width) - 30.0f - float(bit) * 18.0f, 25.0f, 5.0f, p);
    }

    // 底部：进度轨道 + 已播蓝条。
    const float barX = 24.0f;
    const float barW = float(width) - 48.0f;
    const float barY = float(height) - 26.0f;
    const float barH = 8.0f;
    p.setColor(SkColorSetARGB(70, 255, 255, 255));
    canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(barX, barY, barW, barH), 4.0f, 4.0f), p);
    if (t > 0.0) {
        p.setColor(SkColorSetARGB(255, 120, 220, 255));
        canvas->drawRRect(SkRRect::MakeRectXY(
                                  SkRect::MakeXYWH(barX, barY, std::max(barH, barW * float(t)), barH),
                                  4.0f, 4.0f),
                          p);
    }

    return surface->makeImageSnapshot();
}

/// 进程内只生成一次。空返回 = 生成失败（页面会把错误显示出来）。
inline const std::vector<uint8_t>& demoVideoBytes() {
    static const std::vector<uint8_t> kBytes = [] {
        constexpr int kWidth = 480;
        constexpr int kHeight = 270;
        constexpr int kFrames = 40;
        constexpr int kFps = 10;

        std::vector<std::vector<uint8_t>> jpegs;
        jpegs.reserve(kFrames);
        for (int i = 0; i < kFrames; ++i) {
            const sk_sp<SkImage> image = renderDemoFrame(i, kFrames, kWidth, kHeight);
            if (!image)
                return std::vector<uint8_t>{};
            SkJpegEncoder::Options options;
            options.fQuality = 85;
            const sk_sp<SkData> data = SkJpegEncoder::Encode(nullptr, image.get(), options);
            if (!data)
                return std::vector<uint8_t>{};
            const auto* raw = static_cast<const uint8_t*>(data->data());
            jpegs.emplace_back(raw, raw + data->size());
        }
        // 带一条 4 秒立体声 PCM：这一页就是本仓「视频带声音」的活样本，
        // MF / FFmpeg 两条路线都吃得到真音轨（设备起不来时自动退回无声）。
        return buildMjpegAvi(jpegs, kWidth, kHeight, kFps,
                             buildDemoAudio(44100, 2, kFrames / kFps));
    }();
    return kBytes;
}

}  // namespace video_demo
}  // namespace gallery
}  // namespace cui
