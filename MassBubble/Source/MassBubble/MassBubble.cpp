#include "MassBubble.h"

#include "Core/MassBubbleBoot.h"
#include "Core/MassBubbleMemory.h"

#include "Modules/ModuleManager.h"

class FMassBubbleModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		MassBubbleBoot::Mark(TEXT("Module.MassBubble.Startup"));
		MassBubbleMem::PublishCsvMetadata();
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FMassBubbleModule, MassBubble, "MassBubble");

DEFINE_LOG_CATEGORY(LogMassBubble);
