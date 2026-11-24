#include "Modules/ModuleManager.h"

#include "GaussianPointSplatLog.h"
#include "GaussianPointSplatPicking.h"
#include "RenderingThread.h"
#include "DynamicRHI.h"
#include "Containers/Ticker.h"
#include "GaussianPointSplatViewExtension.h"
#include "Engine/Engine.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "SceneViewExtension.h"
#include "ShaderCore.h"

DEFINE_LOG_CATEGORY(LogGaussianPointSplatting);

class FGaussianPointSplattingRuntimeModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GaussianPointSplatting"));
		if (Plugin.IsValid())
		{
			AddShaderSourceDirectoryMapping(TEXT("/Plugin/GaussianPointSplatting"), Plugin->GetBaseDir() / TEXT("Shaders"));
			UE_LOG(LogGaussianPointSplatting, Display, TEXT("Registered shader directory: %s"), *Plugin->GetBaseDir());
		}

		PollHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
		{
			if (FGaussianPointSplatPicking::HasRequests())
			{
				ENQUEUE_RENDER_COMMAND(GPSPollPendingPicks)([](FRHICommandListImmediate&) { FGaussianPointSplatPicking::Poll(); });
			}
			return true;
		}), 0.05f);

		if (GEngine)
		{
			RegisterViewExtension();
		}
		else
		{
			PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FGaussianPointSplattingRuntimeModule::RegisterViewExtension);
			UE_LOG(LogGaussianPointSplatting, Display, TEXT("Deferred Gaussian Point Splatting view extension registration until post engine init."));
		}

		UE_LOG(LogGaussianPointSplatting, Display, TEXT("Gaussian Point Splatting runtime module started."));
	}

	virtual void ShutdownModule() override
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PollHandle);
		PollHandle.Reset();
		if (PostEngineInitHandle.IsValid())
		{
			FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
			PostEngineInitHandle.Reset();
		}

		ViewExtension.Reset();
		ENQUEUE_RENDER_COMMAND(GPSShutdownPicking)([](FRHICommandListImmediate& RHICmdList)
		{
			if (GDynamicRHI && FGaussianPointSplatPicking::HasRequests()) RHICmdList.BlockUntilGPUIdle();
			FGaussianPointSplatPicking::Shutdown();
		});
		FlushRenderingCommands();
		UE_LOG(LogGaussianPointSplatting, Display, TEXT("Gaussian Point Splatting runtime module shut down."));
	}

private:
	void RegisterViewExtension()
	{
		if (PostEngineInitHandle.IsValid())
		{
			FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
			PostEngineInitHandle.Reset();
		}

		if (!ViewExtension.IsValid())
		{
			ViewExtension = FSceneViewExtensions::NewExtension<FGaussianPointSplatViewExtension>();
			UE_LOG(LogGaussianPointSplatting, Display, TEXT("Registered Gaussian Point Splatting view extension."));
		}
	}

	FTSTicker::FDelegateHandle PollHandle;
	FDelegateHandle PostEngineInitHandle;
	TSharedPtr<FGaussianPointSplatViewExtension, ESPMode::ThreadSafe> ViewExtension;
};

IMPLEMENT_MODULE(FGaussianPointSplattingRuntimeModule, GaussianPointSplattingRuntime)
