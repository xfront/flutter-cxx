/// @file test_audio_ring.cpp
/// @brief 各平台出声设备共用的 PCM 环形缓冲（`packages/video/src/audio_ring.h`）契约测试。
///
/// 钉的是「四端 sink 都靠这一份实现算出来的那几件事」，全程不碰任何真设备、
/// 不起线程，所以每一条都是确定性的：
///
///   1. 入队/取出的往返：任何来格式（S16 交织 / F32 交织 / F32 平面）都先归一成
///      F32 交织，再按设备想要的类型落回去；
///   2. **欠载补零也计进消耗** —— 主时钟因此在欠载期间继续按真实速率走；
///   3. `clear()` 的基准语义：位置 = 新基准 + **自那次 clear 起**的消耗。
///      消耗计数不清零的话，每 seek 一次位置就多出一整截「上一段已经播过的时长」，
///      从此永远对不上声音（ALSA 那条真机实测就是这么暴露的）；
///   4. `positionUsMinus()` 扣掉「已交给设备但还没响」的那截，且不越过基准；
///   5. 容量与背压：写满只收一部分（`push` 的返回值），`freeFrames()`/`queuedFrames()`
///      与之一致；跨环形尾界的读写拆段后仍然连续；声明的格式对应指针为空 ⇒ 直接拒收；
///   6. 音量在读取时施加（浮点域），`setVolume()` 夹在 [0,1]；
///   7. `format()` 恒报内部布局（F32 交织），平台侧照着申请设备。
///
/// 采样率取 1000 ⇒ 1 帧采样 = 1000 微秒，微秒↔帧数换算都是整数，断言不需要容差
/// （只有 S16 往返那一条按 ±1 LSB 夹，原因见 `s16RoundTrip()`）。

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "audio_ring.h"

using namespace cui;

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

constexpr int kRate = 1000;        ///< 1 帧 = 1000 微秒
constexpr int kCh = 2;             ///< 立体声（交织步长 2）
constexpr size_t kCapacityFrames = 2000;  ///< reset 时按 capacityMs=2000 算出来

AudioFormat stereo() {
    return AudioFormat{kRate, kCh, AudioSampleFormat::S16Interleaved};
}

/// 一段可预测的 S16 交织素材：样本值 = (帧号 * 7 + 声道) %% 1000 - 500。
std::vector<int16_t> makeS16(size_t frames) {
    std::vector<int16_t> v(frames * size_t(kCh));
    for (size_t f = 0; f < frames; ++f)
        for (int c = 0; c < kCh; ++c)
            v[f * kCh + c] = int16_t((long(f * 7 + c)) % 1000 - 500);
    return v;
}

/// 带「批次标记」的素材：不同批次的值域不重叠，于是「绕回头部的那一段」
/// 能跟「上一批的尾巴」区分开（否则同一个公式生成的两段数值完全一样，断言白写）。
std::vector<int16_t> makeS16Tagged(size_t frames, int tag) {
    std::vector<int16_t> v(frames * size_t(kCh));
    for (size_t f = 0; f < frames; ++f)
        for (int c = 0; c < kCh; ++c)
            v[f * kCh + c] = int16_t(tag * 3000 + long(f) * 5 + c);
    return v;
}

AudioChunk chunkS16(const std::vector<int16_t>& pcm, size_t frames, int64_t ptsUs) {
    AudioChunk c;
    c.frames = frames;
    c.ptsUs = ptsUs;
    c.format = AudioSampleFormat::S16Interleaved;
    c.s16 = pcm.data();
    return c;
}

/// S16 → 环形缓冲（÷32768）→ S16（正样本 ×32767）：正向必然小 1 LSB，是
/// `AudioRing::emit(int16_t*)` 的真实行为，不是 bug ⇒ 比较放到 ±1。
bool s16RoundTrip(const std::vector<int16_t>& a, const std::vector<int16_t>& b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::abs(int(a[i]) - int(b[i])) > 1)
            return false;
    return true;
}

// ── 1. 往返与来格式归一 ─────────────────────────────────────────────────────

void testRoundTrip() {
    std::printf("== 往返：三种来格式都归一成 F32 交织 ==\n");
    AudioRing ring;
    ring.reset(stereo());

    const auto pcm = makeS16(100);
    check(ring.push(chunkS16(pcm, 100, 0)) == 100, "S16 交织 push 全收");
    check(ring.queuedFrames() == 100, "queuedFrames() 跟着涨");
    check(ring.queuedUs() == 100000, "queuedUs() = 帧数 / 采样率");

    std::vector<int16_t> out(100 * kCh, 12345);
    check(ring.readS16(out.data(), 100) == 100, "readS16 取满");
    check(s16RoundTrip(pcm, out), "S16 进 / S16 出（±1 LSB）");
    check(ring.queuedFrames() == 0, "取完就空");
    check(ring.consumedFrames() == 100, "消耗计数 = 取走的帧数");
    check(ring.positionUs() == 100000, "位置 = 消耗 / 采样率");

    // F32 交织：进什么出什么（同一浮点域，不做二次量化）。
    AudioRing f32ring;
    f32ring.reset(AudioFormat{kRate, kCh, AudioSampleFormat::F32Interleaved});
    std::vector<float> src(4 * kCh);
    for (size_t i = 0; i < src.size(); ++i)
        src[i] = 0.25f + float(i) / 64.0f;
    AudioChunk in;
    in.frames = 4;
    in.format = AudioSampleFormat::F32Interleaved;
    in.f32 = src.data();
    check(f32ring.push(in) == 4, "F32 交织 push");
    std::vector<float> dst(4 * kCh, 0.0f);
    f32ring.readF32(dst.data(), 4);
    check(dst == src, "F32 进 / F32 出是逐位相等（音量 1.0 时零换算）");

    // F32 平面：逐声道交织回去。
    AudioRing planar;
    planar.reset(AudioFormat{kRate, kCh, AudioSampleFormat::F32Planar});
    std::vector<float> left{0.1f, 0.2f, 0.3f};
    std::vector<float> right{-0.1f, -0.2f, -0.3f};
    AudioChunk pin;
    pin.frames = 3;
    pin.format = AudioSampleFormat::F32Planar;
    pin.plane[0] = left.data();
    pin.plane[1] = right.data();
    check(planar.push(pin) == 3, "F32 平面 push");
    std::vector<float> inter(3 * kCh, 0.0f);
    planar.readF32(inter.data(), 3);
    check(inter[0] == left[0] && inter[1] == right[0] && inter[4] == left[2] && inter[5] == right[2],
          "平面 → 交织的声道顺序");

    // 平面里某一路为空（超出 7.1 之类）⇒ 那一路保持静音，其余照进。
    AudioRing partial;
    partial.reset(AudioFormat{kRate, kCh, AudioSampleFormat::F32Planar});
    AudioChunk pin2;
    pin2.frames = 2;
    pin2.format = AudioSampleFormat::F32Planar;
    pin2.plane[0] = left.data();
    pin2.plane[1] = nullptr;
    partial.push(pin2);
    std::vector<float> two(2 * kCh, 9.0f);
    partial.readF32(two.data(), 2);
    check(two[0] == left[0] && two[2] == left[1] && two[1] == 0.0f && two[3] == 0.0f,
          "缺的声道补零，不影响别的声道");
}

// ── 2. 欠载补零：静音也占时间 ───────────────────────────────────────────────

void testUnderrunStillTicks() {
    std::printf("== 欠载：补零也计进消耗，主时钟不停 ==\n");
    AudioRing ring;
    ring.reset(stereo());
    const auto pcm = makeS16(40);
    ring.push(chunkS16(pcm, 40, 0));

    std::vector<int16_t> out(100 * kCh, 7);
    check(ring.readS16(out.data(), 100) == 100, "只有 40 帧时也返回请求数");
    check(ring.consumedFrames() == 100, "补的那 60 帧也算消耗");
    check(ring.positionUs() == 100000, "位置按 100 帧推进（不是 40）");
    // 前 40 帧是真数据，后 60 帧必须是零。
    check(s16RoundTrip(std::vector<int16_t>(out.begin(), out.begin() + 40 * kCh),
                       std::vector<int16_t>(pcm.begin(), pcm.begin() + 40 * kCh)),
          "真数据在前、顺序不变");
    bool tailSilent = true;
    for (size_t i = 40 * kCh; i < out.size(); ++i)
        tailSilent = tailSilent && out[i] == 0;
    check(tailSilent, "欠载部分落回静音");

    // 全空时整段都是零，位置照样走（这就是「设备在转、数据没到」那一拍）。
    ring.clear(500000);
    std::vector<int16_t> zeros(50 * kCh, 1);
    ring.readS16(zeros.data(), 50);
    check(zeros.back() == 0, "空队列读出来是静音");
    check(ring.positionUs() == 500000 + 50000, "空队列也在推进位置");
}

// ── 3. clear() 的基准语义（回归钉） ─────────────────────────────────────────

void testClearRebases() {
    std::printf("== clear()：位置 = 新基准 + 自那次 clear 起的消耗 ==\n");
    AudioRing ring;
    ring.reset(stereo());

    // 先正常播 1 秒（seek 之前）。
    const auto pcm = makeS16(1000);
    ring.push(chunkS16(pcm, 1000, 0));
    std::vector<int16_t> sink(1000 * kCh);
    ring.readS16(sink.data(), 1000);
    check(ring.positionUs() == 1000000, "播完 1 秒 ⇒ 位置 1000000");
    check(ring.consumedFrames() == 1000, "消耗计数 1000 帧");

    // seek 到 30 秒：旧实现不清消耗计数 ⇒ 这里会读出 40 秒（本测试钉住它必须是 30 秒）。
    ring.clear(30000000);
    check(ring.positionUs() == 30000000, "clear 之后位置就是基准本身");
    check(ring.consumedFrames() == 0, "消耗计数随 clear 归零");
    check(ring.queuedFrames() == 0, "未播出的采样被丢弃");

    const auto next = makeS16(200);
    ring.push(chunkS16(next, 200, 30000000));
    std::vector<int16_t> more(50 * kCh);
    ring.readS16(more.data(), 50);
    check(ring.positionUs() == 30000000 + 50000, "新基准 + 之后的消耗");

    // 连续两次 seek：不能累加上一次已经播的时长。
    ring.clear(5000000);
    ring.clear(6000000);
    check(ring.positionUs() == 6000000, "连吃两次 clear 只用最后一个基准");

    // 基准为负（回卷到片头之前）也照实返回，不夹到 0 —— 夹取是播放引擎的事。
    ring.clear(-12345);
    check(ring.positionUs() == -12345, "负基准不被偷偷夹住");
}

// ── 4. positionUsMinus()：扣掉在途的那截 ────────────────────────────────────

void testInFlight() {
    std::printf("== positionUsMinus()：已交给设备但没响的不算播出 ==\n");
    AudioRing ring;
    ring.reset(stereo());
    ring.clear(1000000);
    const auto pcm = makeS16(300);
    ring.push(chunkS16(pcm, 300, 0));
    std::vector<int16_t> out(300 * kCh);
    ring.readS16(out.data(), 300);
    check(ring.positionUs() == 1000000 + 300000, "不扣 = 全部算已播出");
    check(ring.positionUsMinus(120) == 1000000 + 180000, "扣 120 帧在途");
    check(ring.positionUsMinus(300) == 1000000, "整段都在途 ⇒ 就是基准");
    check(ring.positionUsMinus(100000) == 1000000, "在途数超过消耗数 ⇒ 夹到基准，不倒退成负数");
    // 采样率未知（还没 reset 成功 / format 无效）时不除零。
    AudioRing cold;
    cold.clear(777);
    check(cold.positionUs() == 777 && cold.positionUsMinus(50) == 777 && cold.queuedUs() == 0,
          "没有有效采样率时只报基准，不做除法");
}

// ── 5. 容量、背压与环形尾界 ─────────────────────────────────────────────────

void testCapacityAndWrap() {
    std::printf("== 容量 / 背压 / 跨尾界读写 ==\n");
    AudioRing ring;
    ring.reset(stereo());  // 默认 capacityMs=2000 ⇒ 2000 帧
    check(ring.freeFrames() == kCapacityFrames, "空队列时 freeFrames = 容量");
    check(ring.format().sampleRate == kRate && ring.format().channels == kCh,
          "format() 报出 reset 时的采样率与声道");
    check(ring.format().format == AudioSampleFormat::F32Interleaved, "format() 恒报内部布局 F32 交织");

    const auto batchA = makeS16Tagged(kCapacityFrames, 1);
    check(ring.push(chunkS16(batchA, kCapacityFrames, 0)) == kCapacityFrames, "正好写满容量");
    const auto over = makeS16Tagged(10, 1);
    AudioChunk extra = chunkS16(over, 10, 0);
    check(ring.push(extra) == 0, "满了就一个帧也不收（非阻塞）");
    check(ring.freeFrames() == 0, "freeFrames 跟着归零");
    check(ring.queuedUs() == int64_t(kCapacityFrames) * 1000, "queuedUs 是队列里的时长");

    // 跨尾界：读走 1500 帧后 head=1500、队列里还剩 A[1500..1999]；再塞 1800 帧的
    // 新批次 ⇒ 只能收下 1500 帧，且从物理 0 号位开始写。下一次读 2000 帧会先
    // 拿到 A 的尾巴、再拿到 B 的头（顺序必须连续，不能错位）。
    std::vector<int16_t> out(1500 * kCh);
    ring.readS16(out.data(), 1500);
    check(ring.freeFrames() == 1500, "读走 1500 后腾出 1500");
    check(s16RoundTrip(out, std::vector<int16_t>(batchA.begin(), batchA.begin() + 1500 * kCh)),
          "先读走的就是开头那 1500 帧");
    const auto batchB = makeS16Tagged(1800, 2);
    check(ring.push(chunkS16(batchB, 1800, 0)) == 1500, "跨过尾界只收得下剩下的 1500 帧");
    std::vector<int16_t> back(2000 * kCh, 0);
    check(ring.readS16(back.data(), 2000) == 2000, "把拆成两段的内容整段读回");
    check(s16RoundTrip(std::vector<int16_t>(back.begin(), back.begin() + 500 * kCh),
                       std::vector<int16_t>(batchA.begin() + 1500 * kCh, batchA.end())),
          "尾界前剩下的 A 尾巴接得上");
    check(s16RoundTrip(std::vector<int16_t>(back.begin() + 500 * kCh, back.end()),
                       std::vector<int16_t>(batchB.begin(), batchB.begin() + 1500 * kCh)),
          "绕回头部的 B 那 1500 帧也接得上");

    // 无效素材别把队列搞脏。
    AudioRing tiny;
    tiny.reset(stereo());
    AudioChunk none;
    none.frames = 0;
    none.s16 = nullptr;
    check(tiny.push(none) == 0, "frames=0 的 chunk 不收");
    AudioChunk wrongPtr;
    wrongPtr.frames = 10;
    wrongPtr.format = AudioSampleFormat::S16Interleaved;
    wrongPtr.s16 = nullptr;
    check(tiny.push(wrongPtr) == 0 && tiny.queuedFrames() == 0,
          "声明了 S16 但指针为空 ⇒ 一个也不收（不能把未定义内容算成数据）");
    std::vector<float> one{0.5f, 0.25f};
    AudioChunk badPlanar;
    badPlanar.frames = 2;
    badPlanar.format = AudioSampleFormat::F32Planar;
    badPlanar.plane[0] = nullptr;
    badPlanar.plane[1] = one.data();
    check(tiny.push(badPlanar) == 0 && tiny.queuedFrames() == 0,
          "平面格式首声道为空同样拒收");
    check(tiny.readS16(nullptr, 10) == 0 && tiny.readF32(nullptr, 10) == 0, "落点为空直接返回 0");
}

// ── 6. 音量在读取时施加 ─────────────────────────────────────────────────────

void testVolume() {
    std::printf("== 音量：读取时相乘，立即生效且不改队列内容 ==\n");
    AudioRing ring;
    ring.reset(stereo());
    std::vector<int16_t> pcm(10 * kCh);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = 10000;
    ring.push(chunkS16(pcm, 10, 0));

    std::vector<int16_t> half(10 * kCh);
    ring.setVolume(0.5f);
    check(ring.volume() == 0.5f, "volume() 读回设置值");
    ring.readS16(half.data(), 10);
    check(std::abs(half[0] - 5000) <= 1, "0.5 音量 ⇒ 采样减半");

    ring.setVolume(2.0f);
    check(ring.volume() == 1.0f, "音量夹上界 1.0");
    ring.setVolume(-3.0f);
    check(ring.volume() == 0.0f, "音量夹下界 0.0");
    std::vector<int16_t> muted(10 * kCh, 1);
    ring.push(chunkS16(pcm, 10, 0));
    ring.readS16(muted.data(), 10);
    bool allSilent = true;
    for (auto v : muted)
        allSilent = allSilent && v == 0;
    check(allSilent, "0 音量 ⇒ 输出全零（浮点域乘，不做二次量化）");

    // F32 落点：音量后仍在 [-1,1]。
    AudioRing f;
    f.reset(AudioFormat{kRate, kCh, AudioSampleFormat::F32Interleaved});
    std::vector<float> src(4 * kCh, 0.8f);
    AudioChunk in;
    in.frames = 4;
    in.format = AudioSampleFormat::F32Interleaved;
    in.f32 = src.data();
    f.push(in);
    f.setVolume(0.5f);
    std::vector<float> dst(4 * kCh, 0.0f);
    f.readF32(dst.data(), 4);
    check(std::abs(dst[0] - 0.4f) < 1e-6, "F32 落点同样施加音量");
}

// ── 7. reset 的边界（容量下限 / 声道下限 / 计数清零） ───────────────────────

void testReset() {
    std::printf("== reset()：换轨（不同采样率/声道）时状态彻底重来 ==\n");
    AudioRing ring;
    ring.reset(stereo());
    ring.push(chunkS16(makeS16(50), 50, 0));
    ring.setVolume(0.3f);

    // 极小采样率 ⇒ 容量仍保底 1024 帧（别让一次 seek 后只写得进几帧）。
    ring.reset(AudioFormat{1, 1, AudioSampleFormat::S16Interleaved});
    check(ring.freeFrames() == 1024, "容量算出来太小 ⇒ 保底 1024 帧");
    check(ring.positionUs() == 0 && ring.consumedFrames() == 0, "reset 清空位置与消耗");
    check(ring.volume() == 0.3f, "reset 只换缓冲布局 ⇒ 音量是设备侧状态，保留");

    // 声道数 0 ⇒ 内部按 1 算，别让 stride=0 把除法和 memcpy 弄成 UB。
    AudioRing mono;
    mono.reset(AudioFormat{kRate, 0, AudioSampleFormat::S16Interleaved});
    check(mono.format().channels == 1, "channels=0 ⇒ 内部夹到 1");
    check(mono.push(chunkS16(makeS16(8), 8, 0)) == 8, "夹完之后能正常入队");

    // capacityMs 由平台侧显式给（AAudio/ALSA 各家缓冲不同），但下限始终是 1024 帧。
    AudioRing small;
    small.reset(stereo(), /*capacityMs=*/100);
    check(small.freeFrames() == 1024, "100ms@1k = 1000 帧 ⇒ 仍被 1024 下限托住");
    AudioRing large;
    large.reset(AudioFormat{8000, kCh, AudioSampleFormat::S16Interleaved}, /*capacityMs=*/500);
    check(large.freeFrames() == 4000, "500ms@8k = 4000 帧（不受下限影响）");
    check(large.format().sampleRate == 8000, "format() 跟着新采样率");
}

}  // namespace

int main() {
    testRoundTrip();
    testUnderrunStillTicks();
    testClearRebases();
    testInFlight();
    testCapacityAndWrap();
    testVolume();
    testReset();

    std::printf("\n%s: %d 条失败\n", g_failures == 0 ? "全部通过" : "有失败", g_failures);
    return g_failures == 0 ? 0 : 1;
}
