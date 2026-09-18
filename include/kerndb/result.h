#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kerndb {

enum class ErrorCode : std::uint16_t {
    kOk = 0,
    kSyntax,
    kBinding,
    kType,
    kInvalidArgument,
    kOutOfRange,
    kOverflow,
    kNotFound,
    kAlreadyExists,
    kResourceExhausted,
    kIo,
    kCorruption,
    kUnsupported,
    kInternal,
};

[[nodiscard]] std::string_view ErrorCodeName(ErrorCode code) noexcept;

struct ErrorContext {
    std::string key;
    std::string value;

    [[nodiscard]] bool operator==(const ErrorContext&) const = default;
};

class Status {
public:
    constexpr Status() noexcept = default;

    [[nodiscard]] static Status Ok() noexcept;
    [[nodiscard]] static Status Error(ErrorCode code, std::string message);

    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] ErrorCode code() const noexcept;
    [[nodiscard]] const std::string& message() const noexcept;
    [[nodiscard]] const std::vector<ErrorContext>& context() const noexcept;
    [[nodiscard]] Status WithContext(std::string key, std::string value) const;
    [[nodiscard]] std::string ToString() const;

private:
    explicit Status(ErrorCode code, std::string message);

    ErrorCode code_{ErrorCode::kOk};
    std::string message_;
    std::vector<ErrorContext> context_;
};

template <typename T>
class [[nodiscard]] Result {
public:
    Result(const T& value)
        : storage_(value) {}

    Result(T&& value)
        : storage_(std::move(value)) {}

    Result(Status status)
        : storage_(std::move(status)) {
        if (std::get<Status>(storage_).ok()) {
            throw std::invalid_argument("a successful Status cannot construct an error Result");
        }
    }

    [[nodiscard]] bool ok() const noexcept {
        return std::holds_alternative<T>(storage_);
    }

    [[nodiscard]] const Status& status() const noexcept {
        if (ok()) {
            static const Status success_status = Status::Ok();
            return success_status;
        }
        return std::get<Status>(storage_);
    }

    [[nodiscard]] const T& value() const& {
        EnsureValue();
        return std::get<T>(storage_);
    }

    [[nodiscard]] T& value() & {
        EnsureValue();
        return std::get<T>(storage_);
    }

    [[nodiscard]] T&& value() && {
        EnsureValue();
        return std::move(std::get<T>(storage_));
    }

private:
    void EnsureValue() const {
        if (!ok()) {
            throw std::logic_error("attempted to read a value from an error Result");
        }
    }

    std::variant<T, Status> storage_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;

    Result(Status status)
        : status_(std::move(status)) {}

    [[nodiscard]] bool ok() const noexcept {
        return status_.ok();
    }

    [[nodiscard]] const Status& status() const noexcept {
        return status_;
    }

private:
    Status status_;
};

}  // namespace kerndb
