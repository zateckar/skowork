// Minimal encrypted remote-withdrawal identities retained independently of database keys.
#pragma once

#include "base/result.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace wb {

struct WithdrawalIdentity {
    std::string issuer, endpoint, subject, kind, remote_id;
    long long expected_revision = 0;
};

struct WithdrawalState {
    bool wipe_pending = false;
    std::vector<WithdrawalIdentity> entries;
    long long revision = 0;
};

class WithdrawalOutbox {
public:
    explicit WithdrawalOutbox(std::filesystem::path file) : file_(std::move(file)) {}
    Result<WithdrawalState> load() const;
    Status save(const WithdrawalState& state) const;
    Status finish_interrupted_wipe(const std::function<Status()>& wipe) const;
    Result<bool> has_item(const std::string& issuer, const std::string& endpoint, const std::string& subject,
                          const std::string& remote_id) const;
    Result<size_t> retry_items(const std::string& issuer, const std::string& endpoint, const std::string& subject,
                               const std::function<Status(const std::string&)>& remove) const;

private:
    Result<WithdrawalState> load_unlocked() const;
    std::filesystem::path file_;
};

}  // namespace wb
