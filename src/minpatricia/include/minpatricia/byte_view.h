#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace minpatricia {

using ByteView = std::span<const std::byte>;

inline ByteView AsBytes(std::string_view value) {
  return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

inline ByteView AsBytes(const std::string& value) {
  return AsBytes(std::string_view(value));
}

inline ByteView AsBytes(const char* value) {
  return AsBytes(std::string_view(value));
}

inline ByteView AsBytes(std::span<const std::uint8_t> value) {
  return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

inline std::string ToString(ByteView value) {
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

inline std::vector<std::byte> CopyBytes(ByteView value) {
  return {value.begin(), value.end()};
}

}  // namespace minpatricia
