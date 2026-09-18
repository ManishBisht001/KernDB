#include "kerndb/result.h"

#include <stdexcept>

namespace kerndb {

std::string_view ErrorCodeName(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::kOk:
            return "ok";
        case ErrorCode::kSyntax:
            return "syntax";
        case ErrorCode::kBinding:
            return "binding";
        case ErrorCode::kType:
            return "type";
        case ErrorCode::kInvalidArgument:
            return "invalid_argument";
        case ErrorCode::kOutOfRange:
            return "out_of_range";
        case ErrorCode::kOverflow:
            return "overflow";
        case ErrorCode::kNotFound:
            return "not_found";
        case ErrorCode::kAlreadyExists:
            return "already_exists";
        case ErrorCode::kResourceExhausted:
            return "resource_exhausted";
        case ErrorCode::kIo:
            return "io";
        case ErrorCode::kCorruption:
            return "corruption";
        case ErrorCode::kUnsupported:
            return "unsupported";
        case ErrorCode::kInternal:
            return "internal";
    }

    return "unknown";
}

Status Status::Ok() noexcept {
    return Status{};
}

Status Status::Error(ErrorCode code, std::string message) {
    if (code == ErrorCode::kOk) {
        throw std::invalid_argument("Status::Error requires a non-success error code");
    }
    return Status(code, std::move(message));
}

bool Status::ok() const noexcept {
    return code_ == ErrorCode::kOk;
}

ErrorCode Status::code() const noexcept {
    return code_;
}

const std::string& Status::message() const noexcept {
    return message_;
}

const std::vector<ErrorContext>& Status::context() const noexcept {
    return context_;
}

Status Status::WithContext(std::string key, std::string value) const {
    if (key.empty()) {
        throw std::invalid_argument("Status context keys cannot be empty");
    }

    Status copy = *this;
    copy.context_.push_back(ErrorContext{
        .key = std::move(key),
        .value = std::move(value),
    });
    return copy;
}

std::string Status::ToString() const {
    std::string output{ErrorCodeName(code_)};
    if (!message_.empty()) {
        output += ": ";
        output += message_;
    }

    for (const ErrorContext& item : context_) {
        output += " [";
        output += item.key;
        output += "=";
        output += item.value;
        output += "]";
    }
    return output;
}

Status::Status(ErrorCode code, std::string message)
    : code_(code),
      message_(std::move(message)) {}

}  // namespace kerndb
