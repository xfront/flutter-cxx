/// @file foundation/key.cpp
/// @brief GlobalKey 注册表实现。

#include "cui/foundation/key.h"

#include <atomic>

namespace cui {

uint64_t GlobalKey::nextId() {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1);
}

}  // namespace cui_core
