// PS3 build: absl::flat_hash_set stand-in backed by std::unordered_set.
#pragma once

#include <absl/hash/hash.h>
#include <unordered_set>

namespace absl {
template <typename K, typename Hash = absl::Hash<K>, typename Eq = std::equal_to<K>,
          typename Alloc = std::allocator<K>>
using flat_hash_set = std::unordered_set<K, Hash, Eq, Alloc>;

template <typename K, typename H, typename E, typename A, typename Pred>
typename std::unordered_set<K, H, E, A>::size_type erase_if(std::unordered_set<K, H, E, A>& c, Pred pred) {
    return std::erase_if(c, pred);
}
} // namespace absl
