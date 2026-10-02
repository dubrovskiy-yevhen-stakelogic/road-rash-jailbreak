// The process's host (game_host.h).
#include "game_host.h"

#include <stdexcept>

namespace rrgame {

namespace {
std::unique_ptr<GameHost>& HostSlot() {
    static std::unique_ptr<GameHost> host;
    return host;
}
} // namespace

GameHost& Host() {
    if (!HostSlot()) throw std::runtime_error("rrgame: no host (SetHost was not called)");
    return *HostSlot();
}
void SetHost(std::unique_ptr<GameHost> host) { HostSlot() = std::move(host); }
bool HostSet() { return HostSlot() != nullptr; }

} // namespace rrgame
