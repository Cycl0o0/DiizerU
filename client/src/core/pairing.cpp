#include "pairing.h"

namespace core {

bool Pairing::begin(const std::string& device_name) {
    auto s = client_.pair_start(device_name);
    if (!s) {
        error_ = "could not reach relay";
        state_ = State::Idle;
        return false;
    }
    start_ = *s;
    state_ = State::AwaitingUser;
    error_.clear();
    return true;
}

void Pairing::poll() {
    if (state_ != State::AwaitingUser) return;
    auto p = client_.pair_poll(start_.device_code);
    if (!p) {
        // transient transport error — stay awaiting, UI will retry next tick
        return;
    }
    switch (p->status) {
        case PairStatus::Approved:
            if (p->relay_session_token.empty()) {
                error_ = "approved but no token";
                state_ = State::Failed;
                return;
            }
            store_.save_token(p->relay_session_token);
            client_.set_bearer(p->relay_session_token);
            state_ = State::Paired;
            break;
        case PairStatus::Denied:
            error_ = "not invited / denied";
            state_ = State::Failed;
            break;
        case PairStatus::Expired:
            error_ = "code expired";
            state_ = State::Failed;
            break;
        case PairStatus::Pending:
        case PairStatus::Unknown:
            break; // keep waiting
    }
}

} // namespace core
