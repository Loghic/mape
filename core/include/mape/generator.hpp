#ifndef MAPE_GENERATOR_HPP
#define MAPE_GENERATOR_HPP

#include <coroutine>
#include <cstddef>
#include <exception>
#include <iterator>
#include <utility>

namespace mape {

// A minimal C++20 coroutine generator (plan §15.4). `co_yield` produces values
// lazily; the generator exposes a standard input-iterator pair so it composes
// with range-based for and the ranges/views machinery, without ever
// materialising the full sequence. (This is the hand-rolled C++20 stand-in for
// C++23's std::generator.)
template <typename T>
class generator {
public:
    struct promise_type {
        T current_{};
        std::exception_ptr error_;

        generator get_return_object() {
            return generator{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(T value) noexcept {
            current_ = std::move(value);
            return {};
        }
        void return_void() noexcept {}
        void unhandled_exception() { error_ = std::current_exception(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit generator(handle_type h) : handle_(h) {}
    generator(generator&& other) noexcept
        : handle_(std::exchange(other.handle_, {})) {}
    generator& operator=(generator&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }
    generator(const generator&) = delete;
    generator& operator=(const generator&) = delete;
    ~generator() {
        if (handle_) handle_.destroy();
    }

    // --- standard input iterator -------------------------------------
    struct iterator {
        using iterator_category = std::input_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        handle_type handle_ = nullptr;

        iterator& operator++() {
            handle_.resume();
            if (handle_.done()) {
                auto err = handle_.promise().error_;
                handle_ = nullptr;
                if (err) std::rethrow_exception(err);
            }
            return *this;
        }
        void operator++(int) { ++(*this); }
        reference operator*() const { return handle_.promise().current_; }
        pointer operator->() const { return &handle_.promise().current_; }
        bool operator==(std::default_sentinel_t) const {
            return handle_ == nullptr || handle_.done();
        }
        bool operator!=(std::default_sentinel_t s) const {
            return !(*this == s);
        }
    };

    iterator begin() {
        if (handle_) {
            handle_.resume();
            if (handle_.done()) {
                auto err = handle_.promise().error_;
                if (err) std::rethrow_exception(err);
                return iterator{nullptr};
            }
        }
        return iterator{handle_};
    }
    std::default_sentinel_t end() noexcept { return {}; }

private:
    handle_type handle_;
};

}  // namespace mape

#endif  // MAPE_GENERATOR_HPP
