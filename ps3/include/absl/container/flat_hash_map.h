// PS3 build: absl::flat_hash_map stand-in backed by std::unordered_map.
#pragma once

#include <absl/hash/hash.h>
#include <unordered_map>

namespace absl {
template <typename K, typename V, typename Hash = absl::Hash<K>, typename Eq = std::equal_to<K>,
          typename Alloc = std::allocator<std::pair<const K, V>>>
using flat_hash_map = std::unordered_map<K, V, Hash, Eq, Alloc>;

template <typename K, typename V, typename H, typename E, typename A, typename Pred>
typename std::unordered_map<K, V, H, E, A>::size_type erase_if(std::unordered_map<K, V, H, E, A>& c, Pred pred) {
    return std::erase_if(c, pred);
}
} // namespace absl
