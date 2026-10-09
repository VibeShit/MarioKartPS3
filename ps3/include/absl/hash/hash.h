// PS3 build: minimal stand-in for absl::Hash, enough for aurora's use of
// absl::flat_hash_map/flat_hash_set. Types can be hashed through std::hash or
// through an AbslHashValue(H, const T&) friend, as with the real library.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace absl {
namespace hash_internal {

inline uint64_t Mix(uint64_t state, uint64_t value) {
    state ^= value + 0x9E3779B97F4A7C15ull + (state << 6) + (state >> 2);
    state ^= state >> 33;
    state *= 0xFF51AFD7ED558CCDull;
    state ^= state >> 29;
    return state;
}

template <typename T, typename = void>
struct HasAbslHashValue : std::false_type {};


template <typename T, typename = void>
struct HasStdHash : std::false_type {};
template <typename T>
struct HasStdHash<T, std::void_t<decltype(std::hash<T>{}(std::declval<const T&>()))>> : std::true_type {};

class HashState {
public:
    explicit HashState(uint64_t state = 0) : m_state(state) {}

    template <typename T, typename... Rest>
    static HashState combine(HashState state, const T& value, const Rest&... rest) {
        state = state.add(value);
        if constexpr (sizeof...(rest) > 0)
            return combine(std::move(state), rest...);
        else
            return state;
    }
    static HashState combine(HashState state) { return state; }

    static HashState combine_contiguous(HashState state, const unsigned char* data, size_t size) {
        uint64_t h = state.m_state;
        size_t i = 0;
        for (; i + 8 <= size; i += 8) {
            uint64_t word;
            std::memcpy(&word, data + i, 8);
            h = Mix(h, word);
        }
        uint64_t tail = 0;
        for (; i < size; ++i)
            tail = (tail << 8) | data[i];
        return HashState(Mix(h, tail ^ size));
    }

    uint64_t value() const { return m_state; }

private:
    template <typename T>
    HashState add(const T& value) const {
        if constexpr (HasAbslHashValue<T>::value) {
            return AbslHashValue(*this, value);
        } else if constexpr (std::is_enum_v<T>) {
            return HashState(Mix(m_state, static_cast<uint64_t>(static_cast<std::underlying_type_t<T>>(value))));
        } else if constexpr (std::is_integral_v<T> || std::is_pointer_v<T>) {
            return HashState(Mix(m_state, static_cast<uint64_t>((uintptr_t)value)));
        } else if constexpr (HasStdHash<T>::value) {
            return HashState(Mix(m_state, static_cast<uint64_t>(std::hash<T>{}(value))));
        } else {
            static_assert(std::has_unique_object_representations_v<T>, "type is not hashable");
            return combine_contiguous(*this, reinterpret_cast<const unsigned char*>(&value), sizeof(T));
        }
    }

    uint64_t m_state;
};

// Declared after HashState is complete; only instantiated from HashState::add.
template <typename T>
struct HasAbslHashValue<T, std::void_t<decltype(AbslHashValue(std::declval<HashState>(), std::declval<const T&>()))>>
    : std::true_type {};

} // namespace hash_internal

template <typename T>
struct Hash {
    size_t operator()(const T& value) const {
        return static_cast<size_t>(hash_internal::HashState::combine(hash_internal::HashState(0x84222325u), value).value());
    }
};

} // namespace absl
