#include "include/configs/sub/RouteUpdater.hpp"
#include "include/engine/EditLocks.h"
#include "include/engine/EngineHost.h"

namespace RouteUpdate {
    bool IsRoutesBeingEdited() {
        return Engine::IsBeingEdited(Engine::EditTarget::Routes);
    }

    void NoteRoutingRestart() {
        if (auto *host = Engine::host()) host->noteRestartNeeded(Engine::RestartReason::Routing);
    }
}
