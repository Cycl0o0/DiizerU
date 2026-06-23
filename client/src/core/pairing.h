// core/pairing — device-code pairing state machine. UI-agnostic: the UI calls
// begin(), shows user_code(), then calls poll() on the relay's interval until
// state() becomes Paired or Failed. On success the relay session token is saved
// to the SessionStore. No SDL/wut here (host-testable).
#pragma once

#include <string>

#include "models.h"
#include "relay_client.h"
#include "session_store.h"

namespace core {

class Pairing {
public:
    enum class State { Idle, AwaitingUser, Paired, Failed };

    Pairing(RelayClient& client, SessionStore& store)
        : client_(client), store_(store) {}

    // Start pairing. Returns false on transport failure (state stays Idle).
    bool begin(const std::string& device_name);

    // Poll once. Call no more often than poll_interval() seconds. Transitions to
    // Paired (token saved + set on client) or Failed (denied/expired).
    void poll();

    State state() const { return state_; }
    const std::string& user_code() const { return start_.user_code; }
    const std::string& verify_url() const { return start_.verify_url; }
    int poll_interval() const { return start_.interval; }
    const std::string& error() const { return error_; }

private:
    RelayClient& client_;
    SessionStore& store_;
    PairStart start_;
    State state_ = State::Idle;
    std::string error_;
};

} // namespace core
