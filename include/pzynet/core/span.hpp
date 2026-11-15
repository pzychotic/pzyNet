#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace pzynet
{

/// Non-owning view over a contiguous sequence (C++17 stand-in for std::span).
template <class T>
class Span
{
public:
    using element_type = T;
    using iterator = T*;

    constexpr Span() noexcept = default;
    constexpr Span(T* data, std::size_t size) noexcept : data_(data), size_(size) {}

    template <std::size_t N>
    constexpr Span(T (&array)[N]) noexcept : data_(array), size_(N)
    {
    }

    /// From any contiguous container exposing data()/size() (std::vector, std::array, std::string, Span<U>).
    template <class C, class = std::enable_if_t<
                           !std::is_array<std::remove_reference_t<C>>::value &&
                           std::is_convertible<decltype(std::declval<C&>().data()), T*>::value &&
                           std::is_convertible<decltype(std::declval<C&>().size()), std::size_t>::value>>
    constexpr Span(C&& container) noexcept : data_(container.data()), size_(container.size())
    {
    }

    constexpr T* data() const noexcept { return data_; }
    constexpr std::size_t size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr T& operator[](std::size_t i) const noexcept { return data_[i]; }
    constexpr T* begin() const noexcept { return data_; }
    constexpr T* end() const noexcept { return data_ + size_; }

    /// Sub-view starting at `offset`; clamped to the view, never out of range.
    constexpr Span subspan(std::size_t offset, std::size_t count = static_cast<std::size_t>(-1)) const noexcept
    {
        if (offset > size_)
            offset = size_;
        const std::size_t rest = size_ - offset;
        return Span(data_ + offset, count < rest ? count : rest);
    }
    constexpr Span first(std::size_t count) const noexcept { return subspan(0, count); }

private:
    T* data_ = nullptr;
    std::size_t size_ = 0;
};

using ByteView = Span<const std::uint8_t>;
using MutableByteView = Span<std::uint8_t>;

} // namespace pzynet
