// PS3 build: absl::btree_map stand-in backed by std::map.
#pragma once
#include <map>
namespace absl {
template <typename K, typename V, typename Compare = std::less<K>,
          typename Alloc = std::allocator<std::pair<const K, V>>>
using btree_map = std::map<K, V, Compare, Alloc>;
} // namespace absl
