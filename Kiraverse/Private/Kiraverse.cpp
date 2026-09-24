#include "Kiraverse.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogKiraverse);

// Plain primary game module; no custom startup/shutdown logic needed.
IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, Kiraverse, "Kiraverse");
