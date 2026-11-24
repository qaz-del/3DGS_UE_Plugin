#include "Modules/ModuleManager.h"

class FGaussianPointSplattingEditorModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override {}
	virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FGaussianPointSplattingEditorModule, GaussianPointSplattingEditor)
