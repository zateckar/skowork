// A small value-or-error type (C++20 has no std::expected). Errors carry a stable code and a message
// that is safe to log: callers never put secrets into it.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace wb {

struct Error {
    std::string code;     // stable, machine-readable ("http", "invalid_grant", "not_found", ...)
    std::string message;  // human-readable detail, no secrets
};

template <typename T>
class Result {
public:
    Result(T value) : v_(std::move(value)) {}            // NOLINT(google-explicit-constructor)
    Result(Error error) : v_(std::move(error)) {}        // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept { return v_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    T& value() & { return std::get<0>(v_); }
    const T& value() const& { return std::get<0>(v_); }
    T&& value() && { return std::get<0>(std::move(v_)); }
    T* operator->() { return &std::get<0>(v_); }
    const T* operator->() const { return &std::get<0>(v_); }
    T& operator*() & { return std::get<0>(v_); }
    const T& operator*() const& { return std::get<0>(v_); }

    [[nodiscard]] const Error& error() const& { return std::get<1>(v_); }

private:
    std::variant<T, Error> v_;
};

// Result for operations without a value.
class Status {
public:
    Status() = default;
    Status(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)
    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] const Error& error() const { return *error_; }

private:
    std::optional<Error> error_;
};

inline Error make_error(std::string code, std::string message = {}) { return Error{std::move(code), std::move(message)}; }

}  // namespace wb
