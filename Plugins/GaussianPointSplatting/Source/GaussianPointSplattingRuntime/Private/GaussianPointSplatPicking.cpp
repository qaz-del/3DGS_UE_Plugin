#include "GaussianPointSplatPicking.h"
#include "GaussianPointSplatAsset.h"
#include "GaussianPointSplatComponent.h"
#include "GaussianPointSplatLog.h"
#include "Async/Async.h"
#include "DrawDebugHelpers.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include <atomic>
#include "Misc/DefaultValueHelper.h"
#include "RenderingThread.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "SceneView.h"
#include "ScreenPass.h"
#include "ShaderParameterStruct.h"

class FGPSPickCS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGPSPickCS);
	SHADER_USE_PARAMETER_STRUCT(FGPSPickCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, PickPixel)
		SHADER_PARAMETER(FIntPoint, PickViewMin)
		SHADER_PARAMETER(FIntPoint, PickViewMax)
		SHADER_PARAMETER(float, MinCoverage)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, PickDepth)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint2>, PickIDTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, PickCoverage)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, PickResult)
	END_SHADER_PARAMETER_STRUCT()
};
BEGIN_SHADER_PARAMETER_STRUCT(FGPSPickReadbackParameters, )
	RDG_BUFFER_ACCESS(Source, ERHIAccess::CopySrc)
END_SHADER_PARAMETER_STRUCT()

IMPLEMENT_GLOBAL_SHADER(FGPSPickCS, "/Plugin/GaussianPointSplatting/Private/GPSFrame.usf", "PickCS", SF_Compute);

namespace
{
	struct FRequest
	{
		FSceneInterface* Scene = nullptr;
		int32 PlayerIndex = INDEX_NONE;
		FVector2f UV;
		float MinCoverage = 0.25f;
		double Deadline = 0.0;
		TFunction<void(const FGaussianPointSplatPickResult&)> Completion;
		TUniquePtr<FRHIGPUBufferReadback> Readback;
		bool bSubmitted = false;
		TArray<FGPSPickOwner> Owners;
	};
	TArray<TUniquePtr<FRequest>> Requests;
	std::atomic<int32> RequestCount{0};
	FAutoConsoleCommandWithWorldAndArgs PickCommand(TEXT("gps.Pick"),
		TEXT("PIE/game only: gps.Pick [U V]. Defaults to screen center (0.5,0.5). Logs stable source ID and draws its Gaussian center."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
			ULocalPlayer* Player = Controller ? Controller->GetLocalPlayer() : nullptr;
			FVector2f UV(0.5f, 0.5f);
			if ((!Args.IsEmpty() && (Args.Num() != 2 || !FDefaultValueHelper::ParseFloat(Args[0], UV.X) || !FDefaultValueHelper::ParseFloat(Args[1], UV.Y))) ||
				!FMath::IsFinite(UV.X) || !FMath::IsFinite(UV.Y) || UV.X < 0 || UV.X >= 1 || UV.Y < 0 || UV.Y >= 1 || !Player || !World->Scene)
			{
				UE_LOG(LogGaussianPointSplatting, Warning, TEXT("gps.Pick requires PIE/game and optional U V in [0,1)."));
				return;
			}
			FSceneInterface* Scene = World->Scene;
			const int32 PlayerIndex = Player->GetControllerId();
			TWeakObjectPtr<UWorld> WeakWorld(World);
			ENQUEUE_RENDER_COMMAND(GPSConsolePick)([Scene, PlayerIndex, UV, WeakWorld](FRHICommandListImmediate&)
			{
				FGaussianPointSplatPicking::Request(Scene, PlayerIndex, UV, 0.25f,
					[WeakWorld](const FGaussianPointSplatPickResult& Hit)
					{
						UE_LOG(LogGaussianPointSplatting, Display, TEXT("GPS Pick: Hit=%d Component=%s Asset=%s Sample=%d FlatID=%d DepthCm=%.3f Coverage=%.3f Error=%s"),
							Hit.bHit, *GetNameSafe(Hit.Component.Get()), *GetPathNameSafe(Hit.SourceAsset.Get()),
							Hit.SampleIndex, Hit.FlatSampleIndex, Hit.ViewDepthCm, Hit.Coverage, *Hit.Error);
						if (Hit.bHit && WeakWorld.IsValid()) DrawDebugSphere(WeakWorld.Get(), Hit.GaussianCenter, 5.0f, 12, FColor::Yellow, false, 3.0f);
					});
			});
		}));
	void Complete(FRequest& Request, FUintVector4 Hit, FString Error)
	{
		if (!Request.Completion) return;
		auto Callback = MoveTemp(Request.Completion);
		auto Owners = MoveTemp(Request.Owners);
		AsyncTask(ENamedThreads::GameThread, [Callback = MoveTemp(Callback), Owners = MoveTemp(Owners), Hit, Error = MoveTemp(Error)]()
		{
			FGaussianPointSplatPickResult Result;
			Result.Error = Error;
			if (Hit.X != 0u)
			{
				for (const FGPSPickOwner& Owner : Owners)
				{
					if (Owner.ComponentID != Hit.X) continue;
					UGaussianPointSplatComponent* Component = Owner.Component.Get();
					UGaussianPointSplatAsset* Root = Owner.RootAsset.Get();
					if (!Component || !Root || Component->Asset != Root || Root->DataRevision != Owner.RootRevision)
					{
						Result.Error = TEXT("Component or asset changed while the GPU query was pending.");
						break;
					}
					for (const FGPSPickSource& Source : Owner.Sources)
					{
						if (Hit.Y < Source.First || Hit.Y - Source.First >= Source.Count) continue;
						UGaussianPointSplatAsset* Asset = Source.Asset.Get();
						const uint32 Index = Hit.Y - Source.First;
						if (!Asset || Asset->DataRevision != Source.Revision || !Asset->Gaussians.IsValidIndex(Index))
						{
							Result.Error = TEXT("Source asset changed while the GPU query was pending.");
							break;
						}
						Result.bHit = true;
						Result.Component = Component;
						Result.SourceAsset = Asset;
						Result.SampleIndex = Index;
						Result.FlatSampleIndex = Hit.Y;
						FMemory::Memcpy(&Result.ViewDepthCm, &Hit.Z, sizeof(float));
						FMemory::Memcpy(&Result.Coverage, &Hit.W, sizeof(float));
						Result.GaussianCenter = FVector(Owner.SourceToWorld.TransformPosition(FVector(Asset->Gaussians[Index].Position)));
						break;
					}
					break;
				}
			}
			Callback(Result);
		});
	}
}

void FGaussianPointSplatPicking::Request(FSceneInterface* Scene, int32 PlayerIndex, FVector2f UV, float MinCoverage,
	TFunction<void(const FGaussianPointSplatPickResult&)> Completion)
{
	check(IsInRenderingThread());
	TUniquePtr<FRequest> Request = MakeUnique<FRequest>();
	Request->Scene = Scene;
	Request->PlayerIndex = PlayerIndex;
	Request->UV = UV;
	Request->MinCoverage = MinCoverage;
	Request->Completion = MoveTemp(Completion);
	Request->Deadline = FPlatformTime::Seconds() + 5.0;
	if (Requests.Num() >= 64) { Complete(*Request, FUintVector4(0,0,0,0), TEXT("Too many pending Gaussian picks.")); return; }
	Requests.Add(MoveTemp(Request));
	RequestCount.store(Requests.Num(), std::memory_order_relaxed);
}

bool FGaussianPointSplatPicking::HasRequests()
{
	return RequestCount.load(std::memory_order_relaxed) != 0;
}

void FGaussianPointSplatPicking::Poll()
{
	check(IsInRenderingThread());
	for (int32 Index = Requests.Num() - 1; Index >= 0; --Index)
	{
		FRequest& Request = *Requests[Index];
		if (Request.Readback && Request.bSubmitted && Request.Readback->IsReady())
		{
			FUintVector4 Hit;
			FMemory::Memcpy(&Hit, Request.Readback->Lock(sizeof(Hit)), sizeof(Hit));
			Request.Readback->Unlock();
			Complete(Request, Hit, FString());
			Requests.RemoveAtSwap(Index);
		}
		else if (FPlatformTime::Seconds() > Request.Deadline)
		{
			Complete(Request, FUintVector4(0,0,0,0), TEXT("Gaussian pick timed out; no matching rendered player view or GPU response."));
			// Keep an in-flight staging buffer alive until its fence completes.
			if (!Request.Readback) Requests.RemoveAtSwap(Index);
		}
	}
	RequestCount.store(Requests.Num(), std::memory_order_relaxed);
}

void FGaussianPointSplatPicking::AddPasses(FRDGBuilder& GraphBuilder, const FSceneView& View,
	const FScreenPassTexture& SceneColor, const FGPSRenderFrame& Frame,
	const TArray<FGPSPickOwner>& Owners)
{
	Poll();
	for (const TUniquePtr<FRequest>& Request : Requests)
	{
		if (Request->Readback || !Request->Completion || Request->Scene != View.Family->Scene ||
			Request->PlayerIndex != View.PlayerIndex || !View.bIsGameView || View.StereoViewIndex != INDEX_NONE) continue;
		Request->Owners = Owners;
		Request->Readback = MakeUnique<FRHIGPUBufferReadback>(TEXT("GPS.ScreenPick"));
		FRDGBufferRef Result = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FUintVector4), 1), TEXT("GPS.PickResult"));
		auto* Parameters = GraphBuilder.AllocParameters<FGPSPickCS::FParameters>();
		Parameters->PickPixel = SceneColor.ViewRect.Min + FIntPoint(
			FMath::FloorToInt(Request->UV.X * SceneColor.ViewRect.Width()), FMath::FloorToInt(Request->UV.Y * SceneColor.ViewRect.Height()));
		Parameters->PickViewMin = SceneColor.ViewRect.Min;
		Parameters->PickViewMax = SceneColor.ViewRect.Max;
		Parameters->MinCoverage = Request->MinCoverage;
		Parameters->PickDepth = Frame.Depth;
		Parameters->PickIDTexture = Frame.ID;
		Parameters->PickCoverage = Frame.Coverage;
		Parameters->PickResult = GraphBuilder.CreateUAV(Result);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GPS Screen Pick"),
			TShaderMapRef<FGPSPickCS>(GetGlobalShaderMap(View.GetFeatureLevel())), Parameters, FIntVector(1,1,1));
		auto* Copy = GraphBuilder.AllocParameters<FGPSPickReadbackParameters>();
		Copy->Source = Result;
		FRequest* Pending = Request.Get();
		GraphBuilder.AddPass(RDG_EVENT_NAME("GPS Pick Readback"), Copy, ERDGPassFlags::Readback,
			[Pending, Result](FRHICommandListImmediate& RHICmdList)
			{
				Pending->Readback->EnqueueCopy(RHICmdList, Result->GetRHI(), sizeof(FUintVector4));
				Pending->bSubmitted = true;
			});
	}
}

void FGaussianPointSplatPicking::Shutdown()
{
	check(IsInRenderingThread());
	for (const TUniquePtr<FRequest>& Request : Requests)
		Complete(*Request, FUintVector4(0,0,0,0), TEXT("Gaussian renderer shut down."));
	Requests.Empty();
	RequestCount.store(0, std::memory_order_relaxed);
}
