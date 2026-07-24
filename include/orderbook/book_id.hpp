#pragma once

#include <cstddef>
#include <cstdint>

namespace orderbook {

inline constexpr std::size_t book_count{4};

// Strong identifier for one of the four independent books. An OrderId is unique
// only within a book, so an active order's stable identity is (BookId, OrderId);
// the same OrderId may be active in more than one book at once.
enum class BookId : std::uint8_t { book_0 = 0, book_1 = 1, book_2 = 2, book_3 = 3 };

[[nodiscard]] constexpr bool valid(BookId book) noexcept {
    return static_cast<std::size_t>(book) < book_count;
}

[[nodiscard]] constexpr std::size_t index_of(BookId book) noexcept {
    return static_cast<std::size_t>(book);
}

}  // namespace orderbook
