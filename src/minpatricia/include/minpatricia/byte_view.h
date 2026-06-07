#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace minpatricia {

template <class T>
class Span {
 public:
  using element_type = T;
  using value_type = typename std::remove_cv<T>::type;
  using pointer = T*;
  using reference = T&;
  using iterator = pointer;
  using const_iterator = pointer;
  using size_type = std::size_t;

  constexpr Span() = default;
  constexpr Span(pointer data, size_type size) : data_(data), size_(size) {}

  template <class U,
            typename std::enable_if<std::is_convertible<U (*)[], T (*)[]>::value, int>::type = 0>
  constexpr Span(const Span<U>& other) : data_(other.data()), size_(other.size()) {}

  template <std::size_t N>
  constexpr Span(T (&data)[N]) : data_(data), size_(N) {}

  template <class U, std::size_t N,
            typename std::enable_if<std::is_convertible<U (*)[], T (*)[]>::value, int>::type = 0>
  constexpr Span(std::array<U, N>& data) : data_(data.data()), size_(N) {}

  template <class U, std::size_t N,
            typename std::enable_if<std::is_convertible<const U (*)[], T (*)[]>::value, int>::type = 0>
  constexpr Span(const std::array<U, N>& data) : data_(data.data()), size_(N) {}

  template <class U, class Alloc,
            typename std::enable_if<std::is_convertible<U (*)[], T (*)[]>::value, int>::type = 0>
  Span(std::vector<U, Alloc>& data) : data_(data.data()), size_(data.size()) {}

  template <class U, class Alloc,
            typename std::enable_if<std::is_convertible<const U (*)[], T (*)[]>::value, int>::type = 0>
  Span(const std::vector<U, Alloc>& data) : data_(data.data()), size_(data.size()) {}

  [[nodiscard]] constexpr pointer data() const { return data_; }
  [[nodiscard]] constexpr size_type size() const { return size_; }
  [[nodiscard]] constexpr bool empty() const { return size_ == 0; }
  [[nodiscard]] constexpr reference operator[](size_type index) const { return data_[index]; }
  [[nodiscard]] constexpr reference back() const { return data_[size_ - 1]; }
  [[nodiscard]] constexpr iterator begin() const { return data_; }
  [[nodiscard]] constexpr iterator end() const { return data_ + size_; }
  [[nodiscard]] constexpr Span subspan(size_type offset) const {
    return Span(data_ + offset, size_ - offset);
  }
  [[nodiscard]] constexpr Span subspan(size_type offset, size_type count) const {
    return Span(data_ + offset, count);
  }

 private:
  pointer data_ = nullptr;
  size_type size_ = 0;
};

using ByteView = Span<const std::byte>;

inline ByteView AsBytes(std::string_view value) {
  return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

inline ByteView AsBytes(const std::string& value) {
  return AsBytes(std::string_view(value));
}

inline ByteView AsBytes(const char* value) {
  return AsBytes(std::string_view(value));
}

inline ByteView AsBytes(Span<const std::uint8_t> value) {
  return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

inline std::string ToString(ByteView value) {
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

inline std::vector<std::byte> CopyBytes(ByteView value) {
  return {value.begin(), value.end()};
}

}  // namespace minpatricia
