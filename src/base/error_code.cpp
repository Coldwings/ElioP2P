#include "eliop2p/base/error_code.h"
#include <map>

namespace eliop2p {

namespace {

class ElioP2PCategory : public std::error_category {
public:
    const char* name() const noexcept override {
        return "ElioP2P";
    }

    std::string message(int ev) const override {
        return to_string(static_cast<ErrorCode>(ev));
    }
};

const ElioP2PCategory& get_category() {
    static ElioP2PCategory category;
    return category;
}

} // anonymous namespace

std::error_code make_error_code(ErrorCode code) {
    return std::error_code(static_cast<int>(code), get_category());
}

std::string to_string(ErrorCode code) {
    switch (code) {
        case ErrorCode::Success: return "Success";
        case ErrorCode::InvalidArgument: return "Invalid argument";
        case ErrorCode::NotFound: return "Not found";
        case ErrorCode::AlreadyExists: return "Already exists";
        case ErrorCode::Timeout: return "Operation timed out";
        case ErrorCode::Cancelled: return "Operation cancelled";
        case ErrorCode::InternalError: return "Internal error";
        case ErrorCode::NotImplemented: return "Not implemented";
        case ErrorCode::NetworkError: return "Network error";
        case ErrorCode::ConnectionFailed: return "Connection failed";
        case ErrorCode::ConnectionClosed: return "Connection closed";
        case ErrorCode::SendFailed: return "Send failed";
        case ErrorCode::ReceiveFailed: return "Receive failed";
        case ErrorCode::CacheMiss: return "Cache miss";
        case ErrorCode::CacheFull: return "Cache is full";
        case ErrorCode::EvictionFailed: return "Eviction failed";
        case ErrorCode::NodeNotFound: return "Node not found";
        case ErrorCode::TransferFailed: return "Transfer failed";
        case ErrorCode::ChunkNotAvailable: return "Chunk not available";
        case ErrorCode::StorageError: return "Storage error";
        case ErrorCode::AuthenticationFailed: return "Authentication failed";
        case ErrorCode::BucketNotFound: return "Bucket not found";
        case ErrorCode::ObjectNotFound: return "Object not found";
        case ErrorCode::ControlPlaneError: return "Control plane error";
        case ErrorCode::RegistrationFailed: return "Registration failed";
        case ErrorCode::MetadataError: return "Metadata error";
        default: return "Unknown error";
    }
}

ElioP2PError::ElioP2PError(ErrorCode code, const std::string& message)
    : code_(code), message_(to_string(code) + ": " + message) {}

const char* ElioP2PError::what() const noexcept {
    return message_.c_str();
}

} // namespace eliop2p
