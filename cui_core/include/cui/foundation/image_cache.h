#pragma once
/// @file foundation/image_cache.h
/// @brief 图像缓存（pub.dev `extended_image` 的 `CacheImage`/`BaseCacheImage` 等价物）。
///
/// 三层，各自独立可关：
///   1. **解码缓存**（内存 LRU，按条目数淘汰）：key 里带目标尺寸，所以
///      同一张图的「缩略 / 原图」是两条互不干扰的条目。
///   2. **编码字节缓存**（内存 LRU，按字节预算淘汰）：命中就跳过磁盘 IO，
///      仍要解码。
///   3. **磁盘缓存**：网络下载的字节落盘，跨进程复用；按 mtime 做 TTL，
///      `pruneDiskCache()` 清理过期项。
///
/// 线程：`loadImage()` 的后台线程会读写这里，所以全部公开接口都是带锁的。
/// 返回的都是值语义 / `sk_sp` / `std::shared_ptr`，不向外暴露内部容器引用。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <include/core/SkData.h>

#include "cui/foundation/image_data.h"

namespace cui {

/// 已解码图像的内存缓存。key 由调用方构造（含尺寸 ⇒ 不同尺寸不会互相顶掉）。
class DecodedImageCache {
public:
    static DecodedImageCache& instance();

    /// 命中并把该条移到 LRU 队首。
    DecodedImagePtr get(const std::string& key);
    void put(const std::string& key, const DecodedImagePtr& image);
    void remove(const std::string& key);
    void clear();

    size_t count() const;
    size_t maxEntries() const;
    /// n==0 等于关掉这层缓存；缩小 n 会立刻淘汰超额条目。
    void setMaxEntries(size_t n);

private:
    DecodedImageCache() = default;
};

/// 编码字节的内存缓存。
class EncodedDataCache {
public:
    static EncodedDataCache& instance();

    sk_sp<SkData> get(const std::string& key);
    void put(const std::string& key, const sk_sp<SkData>& data);
    void remove(const std::string& key);
    void clear();

    size_t bytes() const;
    size_t maxBytes() const;
    /// 超出预算时从最久未用的开始丢；缩小预算会立刻淘汰。
    void setMaxBytes(size_t n);

private:
    EncodedDataCache() = default;
};

/// 磁盘缓存目录：Windows 用 `%LOCALAPPDATA%\cui_core\images`，POSIX 用
/// `$XDG_CACHE_HOME`/`~/.cache`，Apple 用 `~/Library/Caches`。
/// 宿主（Android/iOS 沙盒）应调 `setDiskCacheDir()` 覆盖。
std::string defaultDiskCacheDir();
void setDiskCacheDir(std::string dir);
/// 当前生效的目录；返回空字符串表示磁盘缓存停用（如 Web 平台）。
std::string diskCacheDir();
/// 是否被宿主显式设置过（未设置过 ⇒ 每次取默认目录，便于测试覆盖环境变量）。
bool diskCacheDirExplicitlySet();

/// 稳定哈希（FNV-1a 64 → 16 位十六进制）：磁盘文件名用它，
/// 这样 url/路径本身不必是合法文件名。
std::string cacheIdFor(const std::string& key);

/// 读缓存文件（不存在 / 过期 / 读失败返回空 vector）。
std::vector<uint8_t> readDiskCache(const std::string& id);
bool writeDiskCache(const std::string& id, const void* data, size_t size);
bool hasDiskCache(const std::string& id);

/// 缓存有效期（天）。<=0 表示永不过期。
void setDiskCacheMaxAgeDays(int days);
int diskCacheMaxAgeDays();
/// 过期即视为未命中。
bool isDiskCacheExpired(const std::string& id);

/// 删除过期条目，返回删掉的个数。
int pruneDiskCache();
/// 当前占用字节数（目录不存在时为 0）。
size_t diskCacheUsageBytes();
/// 清空整个缓存目录（只删本模块写的 `cui_img_*` 文件），返回释放的字节数。
size_t clearDiskCache();

}  // namespace cui_core
