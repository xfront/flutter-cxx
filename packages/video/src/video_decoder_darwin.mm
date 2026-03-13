/// @file video/video_decoder_darwin.mm
/// @brief macOS / iOS 系统媒体栈解码后端（AVFoundation：AVURLAsset + AVAssetReader）。
///
/// 路线一（各端系统 API）在 Apple 端的落点，角色与 Windows 的 Media Foundation、
/// Android 的 AMediaCodec、Linux 的 GStreamer 对齐：**解复用 + 解码交给操作系统的
/// 媒体框架**，本仓只把解出来的像素转成 BGRA、把 PCM 转交给 `AudioSink`
/// （`audio_sink_darwin.cpp`，AudioQueue）。
///
/// 关键实现口径：
///   - **不用 `AVPlayer`**：播放器自带出声与自己的时间轴，而本仓的主时钟是
///     `AudioRing` 的「已消耗帧 / 采样率」（见 audio_ring.h），像素还要拿去喂 Skia。
///     `AVAssetReader` 是同一套解码器的「离线」入口：解复用/解码归系统，取帧归我们，
///     `copyNextSampleBuffer` 阻塞到下一帧 ⇒ 与 `readFrame` 的契约天然对齐
///     （与 FFmpeg／MF／Android 后端同一种线程模型，`pumpDriven()` 保持 false）。
///   - **几何信息全部来自轨道，不靠第一帧**：`naturalSize` / `nominalFrameRate` /
///     `preferredTransform` / `asset.duration` 都是同步可读的（见下面「同步 API」一节），
///     于是 `open()` 不必拉帧就能把 `info_` 填满 —— 这点比 GStreamer 那端干净。
///   - 像素：`kCVPixelFormatType_32BGRA`，内存序 B,G,R,A ⇒ 与 Skia 的 `kBGRA_8888`
///     同序；行距取 `CVPixelBufferGetBytesPerRow`（不假设紧排），alpha 那字节一律或成
///     0xFF（与 MF／Android／GStreamer 同口径）。
///   - PCM：向 `AVAssetReaderTrackOutput` 要 **packed float32 交织**，采样率/声道数按
///     音轨原样填进 outputSettings ⇒ 不做重采样，`audioFormat()` 在打开阶段就有值
///     （引擎 `bindBackendLocked` 的时序要求：先开 sink，再取第一帧）。
///   - **seek 只能重建 reader**：`AVAssetReader` 是一次性对象（started 之后不能改
///     timeRange，也不能倒回去），所以 `seekTo` = `cancelReading` + 用
///     `CMTimeRange(start:)` 新建 reader 与两个 output + `startReading`。这是系统端与
///     FFmpeg 端在 seek 代价上的真实差别：每次 seek 都要重开一次解码器会话。
///     新 reader 的首帧本来就落在目标之后 ⇒ 不需要「丢弃过前的帧」那一步（GStreamer
///     后端要 `dropBeforeUs_` 是因为它的 seek 会退到关键帧起点）。
///   - 旋转：**报 `info_.rotation`，不翻像素**（与 FFmpeg／MF／Android 一致，控件层用
///     `RotatedBox` 补偿，它顺带把布局尺寸换成显示尺寸）。补偿角取 `preferredTransform`
///     的 `atan2(b, a)` 换算成 0/90/180/270。
///   - 内存来源（`VideoSource::memory()`）：AVFoundation 没有「从 NSData 建 asset」的
///     公开入口 ⇒ 走官方的 `AVAssetResourceLoaderDelegate`：给一个自定义 URL，delegate
///     按 `dataRequest` 的游标把区间交出去。 NSData 用
///     `dataWithBytesNoCopy:freeWhenDone:NO` 指向后端自己持有的 vector（它活到析构），
///     所以这条路径不会把整份文件再拷一遍。
///   - 背压：PCM 自持队列超过 2 秒就不再从音频 output 取（与 GStreamer 端同一条），
///     让音频那路解码自己堵着，不丢数据。
///
/// 「同步 API」这一条是本文件最大的版本风险：`tracksWithMediaType:` / `naturalSize` /
/// `nominalFrameRate` / `preferredTransform` / `formatDescriptions` 在新 SDK
/// （macOS 15 / iOS 18）上都标了 deprecated，推荐改成 `loadTracksWithMediaType:` 之类的
/// 异步形式。本仓的 `open()` 本来就在工作线程上同步返回，用异步形式还得把引擎接口改成
/// 可等待 ⇒ 这里继续用同步属性（在新 SDK 上仍然可用），并显式压掉那条告警。若将来
/// Apple 真的移除它们，改法是「在 `open()` 里用信号量等异步回调」，不是把 C++ 接口弄成
/// async。
///
/// 编译要求（`packages/video/CMakeLists.txt` 的 darwin 分支已按此设置）：
///   - 必须 `-fobjc-arc`：文件里所有 ObjC 对象都按 ARC 写（含 C++ 类里那个
///     `CuiDarwinReader*` 成员，析构里显式置 nil，不依赖合成的 `.cxx_destruct`）。
///     CF 对象（`CMSampleBufferRef` / `CMBlockBufferRef`）ARC 不接管：凡 `copy*` 拿到的
///     都配一次 `CFRelease`。
///   - 需要框架：AVFoundation、CoreMedia、CoreVideo、AudioToolbox（出声那半在
///     `audio_sink_darwin.cpp`）。
///
/// 有意未覆盖（排查时先看这里）：
///   - **验证到了哪一步**：这台 Linux 机器没有 macOS/iOS SDK，但用 LLVM 的 clang 可以
///     把本文件当 `objective-c++` + `-fobjc-arc` + `-fobjc-runtime=macosx-11.0` 跑
///     `-fsyntax-only`（自写的 Apple stub 头在 `build-*/tmp/applestub/`）。已验证：
///     ObjC++ 语法与声明结构（@interface/@implementation 配对、ivar、属性读写权限）、
///     **选择器存在性**（写错方法名 = `no visible @interface declares the selector`）、
///     `cui::` 作用域（见上面「ObjC 侧」那条）、ARC 的所有权标注冲突、自有类型与 Skia 调用。
///     已改过的真实拦截例：`contentInformationRequest` 的属性其实叫 `contentType`（不是
///     `dataType`，而且 `dataSize`/`initialDataOffset` 不存在）。
///   - **仍不能验证**（只能在 Mac/iOS 上验）：Apple API 的名字与签名本身 —— stub 与代码
///     来自同一份知识，所以「stub 里写了什么名字」不能证明真 SDK 也这么叫；运行期行为；
///     真 SDK 的头版本差异；内存泄漏（`-fsyntax-only` 不做 analyzer 分析，`copy*` 配
///     `CFRelease` 的配对只能靠人看）。第一次上 Mac 编译时优先确认：上面那批同步属性
///     还在（本文件已用 `#pragma clang diagnostic ignored "-Wdeprecated-declarations"`
///     压住，且 stub 里给它们加了 deprecated 标注 ⇒ 压错了位置在本机就会被查到）、
///     `kCVPixelBufferIOSurfacePropertiesKey` 给空字典在本机上的行为（90/270 那两档
///     旋转方向也在这时候一起验）。
///   - 倍速：不交给系统，倍速仍由引擎的节拍决定 —— 与线程模型下其它后端一致。
///   - 多音轨/多字幕切换、`AVAssetReaderOutputPreparation`（预热）、AirPlay 中断、
///     `AVAudioSession` 的激活与打断恢复（iOS 上属于宿主 App 的职责，本仓不碰 —— 记进
///     AGENTS 的「不可验证边界」）。
///   - 不接 VideoToolbox 裸解码（`VTDecompressionSession`）：那样得自己做解复用，
///     就不算「用平台的 video 能力」了。

#include "cui/video/video_decoder.h"

#if defined(CUI_HAVE_AVFOUNDATION)

#import <dispatch/dispatch.h>

#include <strings.h>  // strcasecmp（扩展名 → UTType 那张表用）

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"  // 见上面「同步 API」一节

#import <AVFoundation/AVFoundation.h>
#import <CoreAudio/CoreAudioTypes.h>
#import <CoreGraphics/CoreGraphics.h>  // CGAffineTransform（rotationFromTransform 的入参）
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>

namespace cui {
namespace {

constexpr int64_t kUsPerSecond = 1000000;
/// 单次交给 sink 的采样帧数（≈42ms @48k，与 Android／GStreamer 后端同口径）。
constexpr size_t kPcmChunkFrames = 2048;
/// PCM 自持队列的高水位（微秒）；超了就停止从音频 output 取（见文件头「背压」）。
constexpr int64_t kPcmBacklogUs = 2 * kUsPerSecond;
/// `CMTimeMakeWithSeconds` 的时间基。600 是 Apple 推荐值（能整除 24/25/30/60）；
/// 微秒 → CMTime 只截断到 1/600 秒，seek 精度足够（seek 本来就落在关键帧上）。
constexpr int32_t kTimeScale = 600;
/// `copyNextSampleBuffer` 返回空但 reader 还在 Reading 时的重试预算（本端不该出现，
/// 出现了也不能当场报 EOF —— 2 秒后仍没有才认流尾）。
constexpr int kEmptySampleRetries = 200;

int64_t cmTimeToUs(CMTime time) {
    if (!CMTIME_IS_VALID(time) || CMTIME_IS_INDEFINITE(time) || time.timescale == 0)
        return -1;
    const double sec = CMTimeGetSeconds(time);
    if (!std::isfinite(sec))
        return -1;
    return int64_t(sec * 1000000.0);
}

CMTime usToCMTime(int64_t us) {
    if (us <= 0)
        return kCMTimeZero;
    return CMTimeMakeWithSeconds(double(us) / 1000000.0, kTimeScale);
}

/// `preferredTransform` → 顺时针补偿角。矩阵形如 {a,b,c,d,tx,ty}，纯旋转时
/// `atan2(b, a)` 就是它的角度（视频坐标系 y 向下，所以正值对应「显示时要顺时针转这么多」
/// = 其它后端 `rotation-degrees` / `MF_MT_VIDEO_ROTATION` 的口径）。非 90 倍数的畸变
/// （少见）夹到最近的直角。
int rotationFromTransform(CGAffineTransform t) {
    const double rad = std::atan2(t.b, t.a);
    int deg = int(std::lround(rad * 180.0 / M_PI));
    deg = ((deg % 360) + 360) % 360;
    return ((deg + 45) / 90 * 90) % 360;
}

std::string utf8(NSString* text) {
    if (text == nil)
        return {};
    const char* cstr = text.UTF8String;
    return cstr != nullptr ? std::string(cstr) : std::string();
}

/// 扩展名 → UTType 标识符（只给内存来源用：`AVAssetResourceLoader` 得让 demuxer 知道
/// 这是哪一类容器）。认不出来就交 nil，让 AVFoundation 自己 typefind。
NSString* dataTypeForExtension(NSString* ext) {
    if (ext == nil)
        return nil;
    static const struct {
        const char* ext;
        const char* type;
    } kMap[] = {{"mp4", "public.mpeg-4"},
                {"m4v", "public.mpeg-4"},
                {"mov", "com.apple.quicktime-movie"},
                {"m4a", "com.apple.mpeg-4-audio"},
                {"mp3", "public.mp3"},
                {"aac", "com.apple.coreaudio-format"},
                {"avi", "public.avi"},
                {"webm", "org.webmproject.webm"},
                {"mkv", "com.apple.matroska-video"}};
    const char* cstr = ext.UTF8String;
    if (cstr == nullptr)
        return nil;
    for (const auto& entry : kMap) {
        if (strcasecmp(cstr, entry.ext) == 0)
            return [NSString stringWithUTF8String:entry.type];
    }
    return nil;
}

}  // namespace
}  // namespace cui

// ── ObjC 侧 ──────────────────────────────────────────────────────────────────
// ObjC 的 `@interface` 不能待在 C++ namespace 里（clang：Objective-C declarations
// may only appear in global scope）⇒ 类名自己带 `CuiDarwin` 前缀；
// ObjC 类名是运行期全局符号，重名会在加载时冲突（与本仓 legacy/flutter 那条 ODR
// 教训同一类问题，只是这里换了张注册表）。
// 代价：上面 `namespace cui { namespace { … } }` 里那批小工具（`cmTimeToUs` /
// `usToCMTime` / `rotationFromTransform` / `dataTypeForExtension` / 常量）在这一区
// **必须写成 `cui::xxx()`** —— 全局作用域看不见它们，而 ObjC 类不是 C++ 类型，
// 连参数上的 ADL 都搭不上桥。

/// 内存来源的喂数据者。整个文件已经在内存里，所以「按需范围读」退化成了
/// 「把请求区间一次性交出去」+ `finishLoading`。
@interface CuiDarwinMemoryLoader : NSObject <AVAssetResourceLoaderDelegate>
- (instancetype)initWithData:(NSData*)data dataType:(NSString*)type;
@end

@implementation CuiDarwinMemoryLoader {
    NSData* _data;
    NSString* _dataType;
}

- (instancetype)initWithData:(NSData*)data dataType:(NSString*)type {
    self = [super init];
    if (self != nil) {
        _data = data;      // 指向后端自己的字节（freeWhenDone:NO），生命周期由 C++ 侧保证
        _dataType = type;  // 可为 nil
    }
    return self;
}

- (BOOL)resourceLoader:(AVAssetResourceLoader*)loader
    shouldWaitForLoadingOfRequestedResource:(AVAssetResourceLoadingRequest*)request {
    (void)loader;
    AVAssetResourceLoadingDataRequest* data = request.dataRequest;
    const NSUInteger total = _data.length;
    if (data != nil) {
        // `currentOffset` = 这一路请求已经交到 AVFoundation 手里的字节数 ⇒ 增量推进，
        // demuxer 反复的 range 请求（先读头再读尾）都按各自的游标答。
        long long offset = data.currentOffset;
        long long want = data.requestedLength;
        if (offset < 0)
            offset = 0;
        if (offset >= (long long)total) {
            want = 0;
        } else if (want > (long long)(total - (NSUInteger)offset)) {
            want = (long long)(total - (NSUInteger)offset);
        }
        if (want > 0) {
            NSData* chunk = [_data subdataWithRange:NSMakeRange((NSUInteger)offset,
                                                                (NSUInteger)want)];
            [data appendData:chunk];
        }
    }
    AVAssetResourceLoadingContentInformationRequest* info = request.contentInformationRequest;
    if (info != nil) {
        // 这个属性叫 `contentType`，收的是 UTI 字符串（上面那张扩展名→UTType 表就是为它准备的）。
        // `contentLength` 是只读侧唯一的长度来源，而 `dataSize` / `initialDataOffset` /
        // `byteRangeAccessSupported` 在本文件那条「整份已在内存」的路径上都不必写。
        info.contentType = _dataType;
        info.contentLength = (unsigned long long)total;
        // `responseContentInformation` 是 iOS 15 / macOS 12 才有的属性，本仓的部署下限
        // 更早 ⇒ 不能无条件写它（会编不过）。留空在「整份文件已在内存」这条路径上可用：
        // demuxer 只靠 contentLength 就能把长度定下来。
    }
    [request finishLoading];
    return YES;
}

- (BOOL)resourceLoader:(AVAssetResourceLoader*)loader
        shouldCancelLoadingForResource:(AVAssetResourceLoadingRequest*)request {
    (void)loader;
    (void)request;
    return YES;  // 没有在途的异步读，直接同意取消。
}

@end

/// AVFoundation 那一侧的全部状态。C++ 后端只通过这里几个方法取数，所有 CF 引用的
/// 释放都关在实现里（`copy*` 拿到的都配 `CFRelease`）。
@interface CuiDarwinReader : NSObject {
    int32_t _audioRate;      // 取自音轨 ASBD，outputSettings 要用两次（首开 + 每次 seek）
    int32_t _audioChannels;
}
@property(nonatomic, strong) AVURLAsset* asset;
@property(nonatomic, strong) AVAssetTrack* videoTrack;
@property(nonatomic, strong) AVAssetTrack* audioTrack;
@property(nonatomic, strong) CuiDarwinMemoryLoader* loader;
@property(nonatomic, strong) dispatch_queue_t loaderQueue;
@property(nonatomic, strong) AVAssetReader* reader;
@property(nonatomic, strong) AVAssetReaderTrackOutput* videoOut;
@property(nonatomic, strong) AVAssetReaderTrackOutput* audioOut;

/// 建 asset + 取轨 + 起 reader（从 0 开始）。`info` / `audioFormat` 全部在这里填满。
- (BOOL)openSource:(const cui::VideoSource&)source
              info:(cui::VideoInfo&)info
       audioFormat:(cui::AudioFormat&)audioFormat
             error:(NSString**)error;
/// 重建 reader 从 `us` 处开始（一次性对象的 seek 只能这么做）。
- (BOOL)seekToUs:(int64_t)us error:(NSString**)error;
/// 取一帧：1 = 有帧（已填进 `out`），0 = 流尾，-1 = 出错（`*error` 已填）。
- (int)nextFrame:(cui::VideoFrame&)out error:(NSString**)error;
/// 取一段 PCM（float 交织）追加到 `dst` 尾部。返回追加的帧数；`*ptsUs` 给出这次
/// 追加起点的媒体时间（拿不到时保持 -1，由调用方用 seek 目标兜底）。
- (size_t)appendAudioFramesInto:(std::vector<uint8_t>&)dst
                     frameBytes:(int64_t)frameBytes
                          ptsUs:(int64_t*)ptsUs;
/// 样本 → `VideoFrame`。`nextFrame:` 在它返回后才 `CFRelease`，所以引用不归它管。
/// （必须在这里声明：ObjC 方法只有**先定义**才算先声明，而 `nextFrame:` 排在它前面。）
- (int)frameFromSample:(CMSampleBufferRef)sample
                  into:(cui::VideoFrame&)out
                 error:(NSString**)error;
- (void)teardown;
@end

@implementation CuiDarwinReader

- (void)teardown {
    [_reader cancelReading];
    _reader = nil;
    _videoOut = nil;
    _audioOut = nil;
    _videoTrack = nil;
    _audioTrack = nil;
    if (_loader != nil) {
        [_asset.resourceLoader setDelegate:nil queue:NULL options:0];
        _loader = nil;
    }
    _asset = nil;
    _loaderQueue = nil;
    _audioRate = 0;
    _audioChannels = 0;
}

- (BOOL)buildReaderFromUs:(int64_t)us error:(NSString**)error {
    NSError* err = nil;
    // 一律用 `alloc/init` 而不是 `+assetReaderWithAsset:error:` 那类较新的工厂方法 ——
    // 本仓的 darwin 部署下限早于那批方法，而 init 形式从 iPhone OS 4.0 就在。
    AVAssetReader* reader = [[AVAssetReader alloc] initWithAsset:_asset error:&err];
    if (reader == nil) {
        if (error != nullptr)
            *error = err != nil ? err.localizedDescription : @"AVAssetReader 建不起来";
        return NO;
    }
    if (us > 0)
        reader.timeRange = CMTimeRangeMake(cui::usToCMTime(us), kCMTimePositiveInfinity);

    NSDictionary* videoSettings = @{
        (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
        // 空字典 = 「要 IOSurface，其余默认」。硬件解码器在 iOS 上基本只产 IOSurface
        // 缓冲，而 `CVPixelBufferLockBaseAddress` 对它同样给得出 CPU 地址；不给这个键
        // 在部分机型上会建不起 output 或退回软件路径。
        (id)kCVPixelBufferIOSurfacePropertiesKey : @{}
    };
    AVAssetReaderTrackOutput* videoOut = [[AVAssetReaderTrackOutput alloc]
        initWithTrack:_videoTrack outputSettings:videoSettings];
    if (videoOut == nil || ![reader canAddOutput:videoOut]) {
        if (error != nullptr)
            *error = @"AVAssetReader 不接受这个视频输出（解码器给不出 32BGRA？）";
        return NO;
    }
    [reader addOutput:videoOut];

    if (_audioTrack != nil && _audioRate > 0 && _audioChannels > 0) {
        NSDictionary* audioSettings = @{
            AVFormatIDKey : @(kAudioFormatLinearPCM),
            AVSampleRateKey : @(_audioRate),
            AVNumberOfChannelsKey : @(_audioChannels),
            AVLinearPCMBitDepthKey : @32,
            AVLinearPCMIsFloatKey : @(YES),
            AVLinearPCMIsNonInterleaved : @(NO),
            AVLinearPCMIsBigEndianKey : @(NO)
        };
        AVAssetReaderTrackOutput* audioOut = [[AVAssetReaderTrackOutput alloc]
            initWithTrack:_audioTrack outputSettings:audioSettings];
        // 加不上就只出声这路没了（视频不受影响）：主时钟退回引擎的墙钟节拍由上层决定。
        if (audioOut != nil && [reader canAddOutput:audioOut]) {
            [reader addOutput:audioOut];
            _audioOut = audioOut;
        } else {
            _audioOut = nil;
        }
    }

    if (![reader startReading]) {
        if (error != nullptr)
            *error = reader.error != nil ? reader.error.localizedDescription
                                         : @"AVAssetReader startReading 失败";
        return NO;
    }
    _reader = reader;
    _videoOut = videoOut;
    return YES;
}

- (BOOL)openSource:(const cui::VideoSource&)source
              info:(cui::VideoInfo&)info
       audioFormat:(cui::AudioFormat&)audioFormat
             error:(NSString**)error {
    NSData* data = nil;
    NSURL* url = nil;
    NSString* ext = nil;  // 小写扩展名，只给内存来源用
    if (!source.bytes.empty()) {
        // 不复制整份：`freeWhenDone:NO` 的 NSData 只引用后端自己那块 vector，而它活到
        // 后端析构；`appendData:` 交给 AVFoundation 的那份是逐次新建的小片。
        data = [NSData dataWithBytesNoCopy:const_cast<void*>(static_cast<const void*>(
                                                          source.bytes.data()))
                                    length:(NSUInteger)source.bytes.size()
                              freeWhenDone:NO];
        ext = source.formatHint.length() > 0
                      ? [[[NSString alloc] initWithUTF8String:source.formatHint.c_str()]
                              lowercaseString]
                      : @"mp4";
        // 内存来源没有真实文件名 ⇒ 把扩展名编进那个自定义 URL 的路径后缀：AVURLAsset
        // 按后缀推容器类型，而下面 `contentInformationRequest.dataType` 还会再给一次
        // （两条都是官方口径，互为兜底 —— 所以不需要较新的 `AVURLAssetURLExtensionHint`，
        //  那个键在旧 SDK 上不存在，写死它等于给整条 darwin 路线加一个版本下限）。
        url = [NSURL URLWithString:[NSString stringWithFormat:@"cui-memory:///video.%@", ext]];
    } else {
        if (source.uri.empty()) {
            if (error != nullptr)
                *error = @"空的 VideoSource";
            return NO;
        }
        NSString* ns = [NSString stringWithUTF8String:source.uri.c_str()];
        if ([ns rangeOfString:@"://"].location != NSNotFound)
            url = [NSURL URLWithString:ns];
        else
            url = [NSURL fileURLWithPath:ns];  // 裸路径当文件（与 VideoSource::File 同义）
        if (url == nil) {
            if (error != nullptr)
                *error = @"URL 建不起来";
            return NO;
        }
    }

    // 时长要准的：本文件的 `info.duration` 直接读 `asset.duration`，而默认那条快速路径
    // 只取容器头部给的第一份估计（很多 MP4 因此差一帧）。这一族没有第二处需要它。
    NSDictionary* options = @{AVURLAssetPreferPreciseDurationAndTimingKey : @YES};
    _asset = [[AVURLAsset alloc] initWithURL:url options:options];
    if (data != nil) {
        _loader = [[CuiDarwinMemoryLoader alloc] initWithData:data
                                                     dataType:cui::dataTypeForExtension(ext)];
        _loaderQueue = dispatch_queue_create("cui.video.memory", DISPATCH_QUEUE_SERIAL);
        // options=0：不走「跨进程复用等待」那条语义 —— 喂数据者就在同进程、同一份内存里。
        [_asset.resourceLoader setDelegate:_loader queue:_loaderQueue options:0];
    }

    _videoTrack = [[_asset tracksWithMediaType:AVMediaTypeVideo] firstObject];
    if (_videoTrack == nil) {
        if (error != nullptr)
            *error = @"这个媒体里没有视频轨";
        return NO;
    }
    _audioTrack = [[_asset tracksWithMediaType:AVMediaTypeAudio] firstObject];

    const CGSize size = _videoTrack.naturalSize;
    info.width = (int)size.width;
    info.height = (int)size.height;
    info.frameRate = (double)_videoTrack.nominalFrameRate;
    info.rotation = cui::rotationFromTransform(_videoTrack.preferredTransform);
    const int64_t durationUs = cui::cmTimeToUs(_asset.duration);
    if (durationUs > 0)
        info.duration = std::chrono::microseconds(durationUs);

    // 音轨的采样率/声道数取自它自己的 ASBD —— 必须在 `open()` 就定下来，因为引擎先用
    // 它 `sink->open()`，再开始取帧。
    _audioRate = 0;
    _audioChannels = 0;
    if (_audioTrack != nil) {
        for (id object in _audioTrack.formatDescriptions) {
            CMFormatDescriptionRef fd = (__bridge CMFormatDescriptionRef)object;
            const AudioStreamBasicDescription* asbd = CMAudioFormatDescriptionGetStreamDescription(fd);
            if (asbd != nullptr && asbd->mSampleRate > 0 && asbd->mChannelsPerFrame > 0) {
                _audioRate = (int32_t)asbd->mSampleRate;
                _audioChannels = (int32_t)asbd->mChannelsPerFrame;
                break;
            }
        }
        if (_audioRate <= 0 || _audioChannels <= 0)
            _audioTrack = nil;  // 认不出格式 ⇒ 当作没有音轨（引擎退回无声 + 墙钟）
        else
            audioFormat = cui::AudioFormat{_audioRate, _audioChannels,
                                           cui::AudioSampleFormat::F32Interleaved};
    }
    info.hasAudio = _audioTrack != nil;

    NSString* buildError = nil;
    if (![self buildReaderFromUs:0 error:&buildError]) {
        if (error != nullptr)
            *error = buildError != nil ? buildError : @"AVAssetReader 起不来";
        return NO;
    }
    return YES;
}

- (BOOL)seekToUs:(int64_t)us error:(NSString**)error {
    [_reader cancelReading];
    _reader = nil;
    _videoOut = nil;
    _audioOut = nil;
    NSString* buildError = nil;
    if (![self buildReaderFromUs:us error:&buildError]) {
        if (error != nullptr)
            *error = buildError;
        return NO;
    }
    return YES;
}

- (int)nextFrame:(cui::VideoFrame&)out error:(NSString**)error {
    if (_reader == nil || _videoOut == nil) {
        if (error != nullptr)
            *error = @"reader 还没起来";
        return -1;
    }
    if (_reader.status == AVAssetReaderStatusFailed) {
        if (error != nullptr)
            *error = _reader.error != nil ? _reader.error.localizedDescription
                                          : @"AVAssetReader 失败";
        return -1;
    }

    for (int attempt = 0; attempt < cui::kEmptySampleRetries; ++attempt) {
        if (_reader.status == AVAssetReaderStatusCompleted ||
            _reader.status == AVAssetReaderStatusCancelled || !_videoOut.hasMoreData)
            return 0;  // 流尾
        CMSampleBufferRef sample = [_videoOut copyNextSampleBuffer];  // +1，ARC 不接管 CF
        if (sample == nil) {
            // Reading 中却拿不到样本：本端不该出现，出现了也不能当 EOF ⇒ 短等一下再来。
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const int result = [self frameFromSample:sample into:out error:error];
        CFRelease(sample);
        return result;
    }
    // 超预算：报成错误而不是「播完了」—— 引擎会把 `error()` 往上送，不会把一段
    // 中途卡住的流当成正常结束（与 GStreamer 后端那条口径一致）。
    if (error != nullptr)
        *error = @"reader 还在 Reading，但连续 2 秒交不出样本";
    return -1;
}

/// 样本 → `VideoFrame`（BGRA 拷贝 + SkImage）。样本的引用由调用方管。
- (int)frameFromSample:(CMSampleBufferRef)sample
                  into:(cui::VideoFrame&)out
                 error:(NSString**)error {
    CVImageBufferRef buffer = CMSampleBufferGetImageBuffer(sample);
    if (buffer == nil) {
        if (error != nullptr)
            *error = @"样本里没有像素缓冲";
        return -1;
    }
    const int width = (int)CVPixelBufferGetWidth(buffer);
    const int height = (int)CVPixelBufferGetHeight(buffer);
    const int64_t ptsUs = cui::cmTimeToUs(CMSampleBufferGetPresentationTimeStamp(sample));

    if (CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) != noErr) {
        if (error != nullptr)
            *error = @"CVPixelBufferLockBaseAddress 失败";
        return -1;
    }
    const uint8_t* base = (const uint8_t*)CVPixelBufferGetBaseAddress(buffer);
    const size_t rowBytes = (size_t)width * 4;
    const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
    if (base == nullptr || width <= 0 || height <= 0 || stride < rowBytes) {
        CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        if (error != nullptr)
            *error = @"像素缓冲没有 CPU 地址或行距不符";
        return -1;
    }
    std::vector<uint8_t> pixels(rowBytes * (size_t)height);
    for (int row = 0; row < height; ++row) {
        const uint32_t* src = (const uint32_t*)(base + stride * (size_t)row);
        uint32_t* dst = (uint32_t*)(pixels.data() + rowBytes * (size_t)row);
        // 32BGRA 的内存序是 B,G,R,A ⇒ 与 Skia 的 kBGRA_8888 同序。硬件解码器通常已经把
        // alpha 写成 0xFF，但 AVFoundation 不承诺 ⇒ 一律补满（与 MF／Android／GStreamer
        // 同口径）。
        for (int col = 0; col < width; ++col)
            dst[col] = src[col] | 0xFF000000u;
    }
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);

    const SkImageInfo imageInfo =
            SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType);
    sk_sp<SkData> data = SkData::MakeWithCopy(pixels.data(), pixels.size());
    if (!data) {
        if (error != nullptr)
            *error = @"SkData 分配失败";
        return -1;
    }
    out.image = SkImages::RasterFromData(imageInfo, std::move(data), rowBytes);
    if (!out.image) {
        if (error != nullptr)
            *error = @"SkImage 创建失败";
        return -1;
    }
    out.ptsUs = ptsUs > 0 ? ptsUs : 0;
    return 1;
}

- (size_t)appendAudioFramesInto:(std::vector<uint8_t>&)dst
                     frameBytes:(int64_t)frameBytes
                          ptsUs:(int64_t*)ptsUs {
    if (_audioOut == nil || frameBytes <= 0 || !_audioOut.hasMoreData)
        return 0;
    CMSampleBufferRef sample = [_audioOut copyNextSampleBuffer];
    if (sample == nil)
        return 0;
    if (ptsUs != nullptr)
        *ptsUs = cui::cmTimeToUs(CMSampleBufferGetPresentationTimeStamp(sample));

    size_t added = 0;
    // 我们钉的是 packed + 交织 ⇒ 只会有一路 AudioBuffer。结构体里那个额外的
    // AudioBuffer 是给 `AudioBufferList.mBuffers[1]` 留的余量（柔性数组在 C++ 里不能写）。
    struct Storage {
        AudioBufferList list;
        AudioBuffer extra;
    };
    Storage storage {};
    CMBlockBufferRef block = nullptr;
    const OSStatus status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sample, nullptr, &storage.list, sizeof(storage), kCFAllocatorDefault,
            kCFAllocatorDefault, 0, nullptr, &block);
    if (status == noErr && storage.list.mNumberBuffers == 1) {
        const AudioBuffer& ab = storage.list.mBuffers[0];
        const size_t frames = (size_t)ab.mDataByteSize / (size_t)frameBytes;
        if (frames > 0 && ab.mData != nullptr) {
            const uint8_t* bytes = (const uint8_t*)ab.mData;
            dst.insert(dst.end(), bytes, bytes + frames * (size_t)frameBytes);
            added = frames;
        }
    }
    if (block != nullptr)
        CFRelease(block);
    CFRelease(sample);
    return added;
}

@end

namespace cui {
namespace {

/// AVFoundation 后端。
///
/// 线程：`readFrame`/`readAudio`/`seekTo`/`flushAudio`/析构都只在播放引擎的工作线程
/// （`AVAssetReader` 这一族允许在非主线程用，本文件不碰任何 UI / `AVAudioSession`）；
/// resource loader 的回调在那条自建串行队列上 —— 它只读 `_data`（指向后端持到析构的
/// 字节），不碰这里任何状态 ⇒ 不需要锁。
class DarwinBackend final : public VideoDecoderBackend {
public:
    DarwinBackend() = default;
    ~DarwinBackend() override { release(); }

    DarwinBackend(const DarwinBackend&) = delete;
    DarwinBackend& operator=(const DarwinBackend&) = delete;

    bool open(const VideoSource& source, std::string& error) {
        @autoreleasepool {
            reader_ = [[CuiDarwinReader alloc] init];
            NSString* nsError = nil;
            if (![reader_ openSource:source
                                info:info_
                         audioFormat:audioFormat_
                               error:&nsError]) {
                error = nsError != nil ? utf8(nsError)
                                       : std::string("AVFoundation 打不开这个来源");
                release();
                return false;
            }
        }
        if (info_.width <= 0 || info_.height <= 0) {
            error = "AVFoundation 报的视频尺寸是 0";
            release();
            return false;
        }
        info_.kind = VideoMediaKind::VideoContainer;
        info_.codec = "avfoundation";
        audioOut_ = info_.hasAudio && audioFormat_.isValid();
        lastSeekTargetUs_ = 0;
        pcmPtsUs_ = 0;
        eof_ = false;
        error_.clear();
        return true;
    }

    const VideoInfo& info() const override { return info_; }
    bool atEnd() const override { return eof_; }
    const std::string& error() const override { return error_; }

    bool hasAudioStream() const override { return audioOut_; }
    AudioFormat audioFormat() const override { return audioFormat_; }

    bool readFrame(VideoFrame& out) override {
        if (reader_ == nil || eof_)
            return false;
        NSString* nsError = nil;
        int result = -1;
        @autoreleasepool { result = [reader_ nextFrame:out error:&nsError]; }
        if (result == 1)
            return true;
        if (result == 0) {
            eof_ = true;  // 流尾：reader 读完，或视频 output 再没有数据
            return false;
        }
        // 出错不算结束：引擎会取 `error()` 报成解码失败（与 GStreamer 后端同口径）。
        error_ = nsError != nil ? utf8(nsError) : std::string("AVFoundation 解帧失败");
        return false;
    }

    bool readAudio(AudioChunk& out) override {
        if (!audioOut_ || reader_ == nil)
            return false;
        fillPcmQueue();
        const int64_t frameBytes = audioFormat_.bytesPerFrame();
        if (frameBytes <= 0 || pcmPos_ >= pcmBuf_.size())
            return false;
        const size_t avail = (pcmBuf_.size() - pcmPos_) / (size_t)frameBytes;
        const size_t want = std::min<size_t>(avail, kPcmChunkFrames);
        const size_t bytes = want * (size_t)frameBytes;
        // `AudioChunk` 的内存由后端持有，到下一次 readAudio/flushAudio 有效；
        // `AudioSink::push()` 立刻拷进环形缓冲，所以这个生命周期够用。
        pcmView_.assign(pcmBuf_.begin() + static_cast<ptrdiff_t>(pcmPos_),
                        pcmBuf_.begin() + static_cast<ptrdiff_t>(pcmPos_ + bytes));
        pcmPos_ += bytes;
        if (pcmPos_ == pcmBuf_.size()) {
            pcmBuf_.clear();
            pcmPos_ = 0;
        }
        out.frames = want;
        out.ptsUs = pcmPtsUs_;
        out.format = AudioSampleFormat::F32Interleaved;
        out.f32 = reinterpret_cast<const float*>(pcmView_.data());
        pcmPtsUs_ += (int64_t)want * kUsPerSecond / (int64_t)audioFormat_.sampleRate;
        return true;
    }

    /// seek 之后：未交出的 PCM 全丢，时间基准搬到目标（引擎随后 `sink->flush(target)`）。
    void flushAudio() override {
        pcmBuf_.clear();
        pcmPos_ = 0;
        pcmPtsUs_ = lastSeekTargetUs_;
    }

    bool seekTo(int64_t ptsUs) override {
        if (reader_ == nil)
            return false;
        NSString* nsError = nil;
        BOOL ok = NO;
        @autoreleasepool { ok = [reader_ seekToUs:ptsUs error:&nsError]; }
        if (!ok) {
            error_ = nsError != nil ? utf8(nsError) : std::string("AVFoundation seek 失败");
            return false;
        }
        lastSeekTargetUs_ = std::max<int64_t>(0, ptsUs);
        eof_ = false;
        error_.clear();
        flushAudio();
        return true;
    }

private:
    void release() {
        if (reader_ != nil) {
            [reader_ teardown];
            reader_ = nil;  // 显式断掉强引用，不依赖 ARC 给 C++ 成员合成的清理
        }
        pcmBuf_.clear();
        pcmView_.clear();
        pcmPos_ = 0;
    }

    /// 把音频 output 里已经解出来的样本全搬进自持队列（有上限；见文件头「背压」）。
    void fillPcmQueue() {
        if (!audioOut_)
            return;
        const int64_t frameBytes = audioFormat_.bytesPerFrame();
        while (pcmBacklogUs() < kPcmBacklogUs) {
            const bool wasEmpty = pcmBuf_.empty();
            int64_t ptsUs = -1;
            size_t got = 0;
            @autoreleasepool {
                got = [reader_ appendAudioFramesInto:pcmBuf_ frameBytes:frameBytes ptsUs:&ptsUs];
            }
            if (got == 0)
                break;  // 这一轮没有了
            if (wasEmpty)
                pcmPtsUs_ = ptsUs > 0 ? ptsUs : lastSeekTargetUs_;
        }
    }

    int64_t pcmBacklogUs() const {
        const int64_t frameBytes = audioFormat_.bytesPerFrame();
        if (frameBytes <= 0 || audioFormat_.sampleRate <= 0 || pcmBuf_.size() <= pcmPos_)
            return 0;
        const int64_t frames = (int64_t)(pcmBuf_.size() - pcmPos_) / frameBytes;
        return frames * kUsPerSecond / (int64_t)audioFormat_.sampleRate;
    }

    CuiDarwinReader* reader_ = nil;  // ARC：C++ 成员默认 __strong

    VideoInfo info_;
    std::string error_;
    bool eof_ = false;

    bool audioOut_ = false;
    AudioFormat audioFormat_;

    // 节拍（seek 目标：PCM 队列的 pts 锚点要用，引擎的 `sink->flush(target)` 也走它）
    int64_t lastSeekTargetUs_ = 0;

    // PCM 队列（一次 readAudio 交一片，与 Android／GStreamer 后端同形）
    std::vector<uint8_t> pcmBuf_;
    std::vector<uint8_t> pcmView_;
    size_t pcmPos_ = 0;
    int64_t pcmPtsUs_ = 0;
};

}  // namespace
}  // namespace cui

#pragma clang diagnostic pop

// ── 工厂与探测 ───────────────────────────────────────────────────────────────

namespace cui {

std::unique_ptr<VideoDecoderBackend> createAVFoundationBackend(const VideoSource& source,
                                                               std::string& error) {
    auto backend = std::make_unique<DarwinBackend>();
    if (!backend->open(source, error))
        return nullptr;
    return backend;
}

/// 运行期探测：Apple 端没有「装了插件才能用」这一层 —— AVFoundation 是系统框架，
/// 链接期就定死了。真正决定成败的是「这台设备的解码器认不认这个码流」，那只有
/// `startReading` 之后才知道，所以这里只能恒报可用（打不开时由 `open()` 的错误文本
/// 如实说明）。`NSClassFromString` 只负责把「框架根本没链上」这种链接配置错误单独照
/// 出来，不留到第一次打开时才炸。
bool avFoundationVideoAvailable(std::string& note) {
    Class cls = NSClassFromString(@"AVURLAsset");
    if (cls == nil) {
        note = "AVFoundation 不在进程里（没链接框架 / 被裁剪过的 SDK）";
        return false;
    }
    return true;
}

}  // namespace cui

#endif  // CUI_HAVE_AVFOUNDATION
