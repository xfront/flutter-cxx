/// @file foundation/image_cache.cpp — LRU 内存缓存 + 磁盘缓存实现。
// getenv / fopen 在这里是刻意用的（缓存目录与临时文件写入），MSVC 的 C4996
// 建议的 _dupenv_s / fopen_s 非标准，会毁掉跨平台可读性。
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "cui/foundation/image_cache.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace fs = std::filesystem;

namespace cui {

namespace {

/// 通用 LRU：`order_` 保存访问顺序（队首=最近使用），`map_` 保存 key→迭代器。
/// 淘汰策略（按条数 / 按字节）留给调用方，避免这里既管容器又管预算。
template <typename K, typename V>
class Lru {
public:
    V* find(const K& key) {
        auto it = map_.find(key);
        if (it == map_.end())
            return nullptr;
        order_.splice(order_.begin(), order_, it->second);
        return &it->second->second;
    }
    /// 插入或覆盖，并置为最近使用。
    void insert(const K& key, V value) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second->second = std::move(value);
            order_.splice(order_.begin(), order_, it->second);
            return;
        }
        order_.emplace_front(key, std::move(value));
        map_.emplace(key, order_.begin());
    }
    /// 丢掉最久未用的一条；`out` 拿到它的值（用于扣减字节预算）。
    bool popOldest(V* out = nullptr) {
        if (order_.empty())
            return false;
        if (out)
            *out = order_.back().second;
        map_.erase(order_.back().first);
        order_.pop_back();
        return true;
    }
    void erase(const K& key) {
        auto it = map_.find(key);
        if (it == map_.end())
            return;
        order_.erase(it->second);
        map_.erase(it);
    }
    void clear() {
        order_.clear();
        map_.clear();
    }
    size_t size() const { return map_.size(); }

private:
    std::list<std::pair<K, V>> order_;
    std::unordered_map<K, typename std::list<std::pair<K, V>>::iterator> map_;
};

struct DecodedState {
    mutable std::mutex mutex;
    Lru<std::string, DecodedImagePtr> lru;
    size_t maxEntries = 48;
};

struct EncodedState {
    mutable std::mutex mutex;
    Lru<std::string, sk_sp<SkData>> lru;
    size_t maxBytes = 64u * 1024u * 1024u;
    size_t bytes = 0;
};

DecodedState& decodedState() {
    static DecodedState s;
    return s;
}
EncodedState& encodedState() {
    static EncodedState s;
    return s;
}

std::string envNonEmpty(const char* name) {
    if (const char* v = std::getenv(name))
        return v;
    return {};
}

std::mutex& dirMutex() {
    static std::mutex m;
    return m;
}

std::string& diskDirSlot() {
    static std::string dir;
    return dir;
}

bool& diskDirExplicitSlot() {
    static bool explicitSet = false;
    return explicitSet;
}

int& maxAgeSlot() {
    static int days = 30;
    return days;
}

/// 缓存文件全路径（目录为空 ⇒ 空串，代表磁盘缓存停用）。
std::string diskCachePath(const std::string& id) {
    const std::string dir = diskCacheDir();
    if (dir.empty())
        return {};
    return dir + "/cui_img_" + id;
}

size_t dataSize(const sk_sp<SkData>& d) { return d ? d->size() : size_t(0); }

}  // namespace

// ── DecodedImageCache ───────────────────────────────────────────────────────

DecodedImageCache& DecodedImageCache::instance() {
    static DecodedImageCache c;
    return c;
}

DecodedImagePtr DecodedImageCache::get(const std::string& key) {
    DecodedState& s = decodedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (DecodedImagePtr* hit = s.lru.find(key))
        return *hit;
    return nullptr;
}

void DecodedImageCache::put(const std::string& key, const DecodedImagePtr& image) {
    if (!image || !image->valid())
        return;
    DecodedState& s = decodedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.maxEntries == 0)
        return;
    s.lru.insert(key, image);
    while (s.lru.size() > s.maxEntries)
        s.lru.popOldest();
}

void DecodedImageCache::remove(const std::string& key) {
    DecodedState& s = decodedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.lru.erase(key);
}

void DecodedImageCache::clear() {
    DecodedState& s = decodedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.lru.clear();
}

size_t DecodedImageCache::count() const {
    const DecodedState& s = decodedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.lru.size();
}

size_t DecodedImageCache::maxEntries() const {
    const DecodedState& s = decodedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.maxEntries;
}

void DecodedImageCache::setMaxEntries(size_t n) {
    DecodedState& s = decodedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.maxEntries = n;
    while (n > 0 && s.lru.size() > n)
        s.lru.popOldest();
    if (n == 0)
        s.lru.clear();
}

// ── EncodedDataCache ────────────────────────────────────────────────────────

EncodedDataCache& EncodedDataCache::instance() {
    static EncodedDataCache c;
    return c;
}

sk_sp<SkData> EncodedDataCache::get(const std::string& key) {
    EncodedState& s = encodedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (sk_sp<SkData>* hit = s.lru.find(key))
        return *hit;
    return nullptr;
}

void EncodedDataCache::put(const std::string& key, const sk_sp<SkData>& data) {
    if (!data)
        return;
    EncodedState& s = encodedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.maxBytes == 0)
        return;
    // 覆盖同一 key 时先扣掉旧值，字节账才不飘（find 顺带把它置为最近使用）。
    if (sk_sp<SkData>* old = s.lru.find(key))
        s.bytes -= dataSize(*old);
    s.lru.insert(key, data);
    s.bytes += data->size();
    const size_t budget = s.maxBytes;
    sk_sp<SkData> evicted;
    while (s.bytes > budget && s.lru.popOldest(&evicted)) {
        s.bytes -= dataSize(evicted);
        evicted.reset();
    }
}

void EncodedDataCache::remove(const std::string& key) {
    EncodedState& s = encodedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (sk_sp<SkData>* old = s.lru.find(key)) {
        s.bytes -= dataSize(*old);
        s.lru.erase(key);
    }
}

void EncodedDataCache::clear() {
    EncodedState& s = encodedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.lru.clear();
    s.bytes = 0;
}

size_t EncodedDataCache::bytes() const {
    const EncodedState& s = encodedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.bytes;
}

size_t EncodedDataCache::maxBytes() const {
    const EncodedState& s = encodedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.maxBytes;
}

void EncodedDataCache::setMaxBytes(size_t n) {
    EncodedState& s = encodedState();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.maxBytes = n;
    sk_sp<SkData> evicted;
    while (n > 0 && s.bytes > n && s.lru.popOldest(&evicted))
        s.bytes -= dataSize(evicted);
    if (n == 0) {
        s.lru.clear();
        s.bytes = 0;
    }
}

// ── 磁盘缓存目录 ────────────────────────────────────────────────────────────

std::string defaultDiskCacheDir() {
#if defined(__EMSCRIPTEN__)
    // Web 没有可用的用户可写目录（要持久化得走 IndexedDB，是另一套东西），
    // 磁盘缓存整体停用。
    return {};
#elif defined(_WIN32)
    std::string base = envNonEmpty("LOCALAPPDATA");
    if (base.empty())
        base = envNonEmpty("TEMP");
    if (base.empty())
        return {};
    return base + "\\cui_core\\images";
#elif defined(__APPLE__)
    std::string home = envNonEmpty("HOME");
    if (home.empty())
        return {};
    return home + "/Library/Caches/cui_core/images";
#else
    std::string xdg = envNonEmpty("XDG_CACHE_HOME");
    if (!xdg.empty())
        return xdg + "/cui_core/images";
    std::string home = envNonEmpty("HOME");
    if (home.empty())
        return {};
    return home + "/.cache/cui_core/images";
#endif
}

void setDiskCacheDir(std::string dir) {
    std::lock_guard<std::mutex> lock(dirMutex());
    diskDirSlot() = std::move(dir);
    diskDirExplicitSlot() = !diskDirSlot().empty();
}

std::string diskCacheDir() {
    std::lock_guard<std::mutex> lock(dirMutex());
    if (diskDirExplicitSlot())
        return diskDirSlot();
    return defaultDiskCacheDir();
}

bool diskCacheDirExplicitlySet() {
    std::lock_guard<std::mutex> lock(dirMutex());
    return diskDirExplicitSlot();
}

void setDiskCacheMaxAgeDays(int days) {
    std::lock_guard<std::mutex> lock(dirMutex());
    maxAgeSlot() = days;
}

int diskCacheMaxAgeDays() {
    std::lock_guard<std::mutex> lock(dirMutex());
    return maxAgeSlot();
}

// ── 磁盘缓存读写 ────────────────────────────────────────────────────────────

std::string cacheIdFor(const std::string& key) {
    // FNV-1a 64：只负责「把任意字符串变成合法文件名」，不承担安全职责。
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : key) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

bool hasDiskCache(const std::string& id) {
    const std::string path = diskCachePath(cacheIdFor(id));
    if (path.empty())
        return false;
    std::error_code ec;
    return fs::exists(path, ec) && !ec;
}

bool isDiskCacheExpired(const std::string& id) {
    const std::string path = diskCachePath(cacheIdFor(id));
    if (path.empty())
        return true;
    std::lock_guard<std::mutex> lock(dirMutex());
    const int days = maxAgeSlot();
    if (days <= 0)
        return false;
    std::error_code ec;
    const auto written = fs::last_write_time(path, ec);
    if (ec)
        return true;
    const auto age =
        std::chrono::duration_cast<std::chrono::hours>(fs::file_time_type::clock::now() - written)
            .count();
    return age > int64_t(days) * 24;
}

std::vector<uint8_t> readDiskCache(const std::string& id) {
    if (isDiskCacheExpired(id))
        return {};
    const std::string path = diskCachePath(cacheIdFor(id));
    if (path.empty())
        return {};
    sk_sp<SkData> data = SkData::MakeFromFileName(path.c_str());
    if (!data)
        return {};
    const uint8_t* p = static_cast<const uint8_t*>(data->data());
    return std::vector<uint8_t>(p, p + data->size());
}

bool writeDiskCache(const std::string& id, const void* data, size_t size) {
    const std::string dir = diskCacheDir();
    if (dir.empty() || !data || size == 0)
        return false;
    const std::string path = diskCachePath(cacheIdFor(id));
    if (path.empty())
        return false;
    std::error_code ec;
    fs::create_directories(dir, ec);
    // 先写临时文件再改名：进程中途被杀不会留下半个缓存文件。
    const std::string tmp = path + ".part";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    const bool wrote = std::fwrite(data, 1, size, f) == size;
    std::fclose(f);
    if (!wrote) {
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        // Windows 上 rename 不覆盖已存在的目标 ⇒ 先删再改。
        std::error_code ec2;
        fs::remove(path, ec2);
        fs::rename(tmp, path, ec);
        if (ec) {
            fs::remove(tmp, ec2);
            return false;
        }
    }
    return true;
}

int pruneDiskCache() {
    std::lock_guard<std::mutex> lock(dirMutex());
    const int days = maxAgeSlot();
    const std::string dir = diskDirExplicitSlot() ? diskDirSlot() : defaultDiskCacheDir();
    if (dir.empty() || days <= 0)
        return 0;
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec)
        return 0;
    int removed = 0;
    for (const fs::directory_entry& e : it) {
        const std::string name = e.path().filename().string();
        if (name.rfind("cui_img_", 0) != 0)
            continue;
        std::error_code ec2;
        const auto written = e.last_write_time(ec2);
        if (ec2)
            continue;
        const auto age =
            std::chrono::duration_cast<std::chrono::hours>(fs::file_time_type::clock::now() - written)
                .count();
        if (age <= int64_t(days) * 24)
            continue;
        std::error_code ec3;
        fs::remove(e.path(), ec3);
        if (!ec3)
            ++removed;
    }
    return removed;
}

size_t diskCacheUsageBytes() {
    const std::string dir = diskCacheDir();
    if (dir.empty())
        return 0;
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec)
        return 0;
    size_t total = 0;
    for (const fs::directory_entry& e : it) {
        const std::string name = e.path().filename().string();
        if (name.rfind("cui_img_", 0) != 0)
            continue;
        std::error_code ec2;
        const auto s = e.file_size(ec2);
        if (!ec2)
            total += size_t(s);
    }
    return total;
}

size_t clearDiskCache() {
    const std::string dir = diskCacheDir();
    if (dir.empty())
        return 0;
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec)
        return 0;
    size_t freed = 0;
    for (const fs::directory_entry& e : it) {
        const std::string name = e.path().filename().string();
        if (name.rfind("cui_img_", 0) != 0)
            continue;
        std::error_code ec2;
        const auto s = e.file_size(ec2);
        if (!ec2)
            freed += size_t(s);
        fs::remove(e.path(), ec2);
    }
    return freed;
}

}  // namespace cui_core
