// SPDX-License-Identifier: GPL-3.0-or-later
//
// The two-level variant Operation and Member are made of (ADR-0059). An
// operation is one of a few families, each family a std::variant of at most
// a dozen operations. One flat std::variant of 62 alternatives cost O(N^2)
// code in every visit, copy and assignment under libstdc++ (alternative N
// sits N unions deep, and UBSan checks every level): 40 minutes for one
// translation unit under ASan+UBSan. Two levels keep each variant short, so
// the code grows with the number of operations, not with its square.
//
// What callers see is still "one of these operations": get_if<AddTrack>(),
// holds<AddTrack>() and visit() reach the leaf through its family, and
// index() counts the leaves in family order. The wire and the session file
// name an operation by its kind, never by an index, so the grouping is
// invisible to both.
#pragma once

#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <variant>

namespace sonora::model::detail {

// Whether F, an alternative of the outer variant, is T or holds T.
template <typename T, typename F>
struct slot_holds : std::is_same<T, F> {};
template <typename T, typename... A>
struct slot_holds<T, std::variant<A...>>
    : std::bool_constant<std::is_same_v<T, std::variant<A...>> || (std::is_same_v<T, A> || ...)> {};

// The outer alternative that is or holds T; void when none does.
template <typename T, typename... Fs>
struct slot_of {
    using type = void;
};
template <typename T, typename F, typename... Fs>
struct slot_of<T, F, Fs...> {
    using type = std::conditional_t<slot_holds<T, F>::value, F, typename slot_of<T, Fs...>::type>;
};

template <typename F>
struct width : std::integral_constant<std::size_t, 1> {};
template <typename... A>
struct width<std::variant<A...>> : std::integral_constant<std::size_t, sizeof...(A)> {};

template <typename F>
struct is_family : std::false_type {};
template <typename... A>
struct is_family<std::variant<A...>> : std::true_type {};

// Fs are the outer alternatives: families (std::variant of leaves) and
// leaves standing alone (Compound).
template <typename... Fs>
class OpVariant {
public:
    using Families = std::variant<Fs...>;
    template <typename T>
    using slot_t = typename slot_of<std::decay_t<T>, Fs...>::type;

    // How many leaves: what std::variant_size was for the flat variant.
    static constexpr std::size_t kinds = (width<Fs>::value + ...);

    OpVariant() = default;

    // From any leaf, or any family, of this set.
    template <typename T, typename S = slot_t<T>, std::enable_if_t<!std::is_void_v<S>, int> = 0>
    OpVariant(T&& x) : v_(std::in_place_type<S>, std::forward<T>(x)) {}

    // The leaf's place counted over every leaf, family by family.
    std::size_t index() const {
        constexpr std::array<std::size_t, sizeof...(Fs)> sizes{width<Fs>::value...};
        std::size_t at = 0;
        for (std::size_t f = 0; f < v_.index(); ++f) at += sizes[f];
        return at + std::visit(
                        [](const auto& s) -> std::size_t {
                            if constexpr (is_family<std::decay_t<decltype(s)>>::value) {
                                return s.index();
                            } else {
                                return 0;
                            }
                        },
                        v_);
    }

    template <typename T>
    const T* get_if() const {
        using S = slot_t<T>;
        static_assert(!std::is_void_v<S>, "not an alternative of this operation set");
        const S* s = std::get_if<S>(&v_);
        if constexpr (std::is_same_v<S, T>) {
            return s;
        } else {
            return s != nullptr ? std::get_if<T>(s) : nullptr;
        }
    }
    template <typename T>
    T* get_if() {
        return const_cast<T*>(static_cast<const OpVariant*>(this)->get_if<T>());
    }
    template <typename T>
    bool holds() const {
        return get_if<T>() != nullptr;
    }

    // `f` called with the leaf: once through the family, then within it.
    template <typename F>
    decltype(auto) visit(F&& f) const& {
        return std::visit(
            [&f](const auto& s) -> decltype(auto) {
                if constexpr (is_family<std::decay_t<decltype(s)>>::value) {
                    return std::visit(f, s);
                } else {
                    return f(s);
                }
            },
            v_);
    }
    template <typename F>
    decltype(auto) visit(F&& f) && {
        return std::visit(
            [&f](auto&& s) -> decltype(auto) {
                if constexpr (is_family<std::decay_t<decltype(s)>>::value) {
                    return std::visit(f, std::move(s));
                } else {
                    return f(std::move(s));
                }
            },
            std::move(v_));
    }

    // The outer variant itself: what code split by family dispatches on.
    const Families& families() const& { return v_; }
    Families&& families() && { return std::move(v_); }

    friend bool operator==(const OpVariant& a, const OpVariant& b) { return a.v_ == b.v_; }
    friend bool operator!=(const OpVariant& a, const OpVariant& b) { return !(a == b); }

private:
    Families v_;
};

}  // namespace sonora::model::detail
