#include "Modules/ModuleManager.h"

class FMyketBillingModule : public IModuleInterface
{
public:
	virtual void StartupModule() override {}
	virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FMyketBillingModule, MyketBilling)
