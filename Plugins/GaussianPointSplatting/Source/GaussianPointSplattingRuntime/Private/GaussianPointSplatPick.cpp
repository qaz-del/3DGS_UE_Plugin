#include "GaussianPointSplatPick.h"
#include "GaussianPointSplatPicking.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "RenderingThread.h"

UGaussianPointSplatPick* UGaussianPointSplatPick::PickGaussianAtScreenUV(APlayerController* PlayerController, FVector2D ScreenUV, float MinCoverage)
{
	UGaussianPointSplatPick* Action = NewObject<UGaussianPointSplatPick>();
	Action->Controller = PlayerController;
	Action->UV = ScreenUV;
	Action->CoverageThreshold = FMath::Clamp(MinCoverage, 0.0f, 1.0f);
	if (PlayerController) Action->RegisterWithGameInstance(PlayerController);
	return Action;
}

void UGaussianPointSplatPick::Activate()
{
	ULocalPlayer* LocalPlayer = Controller ? Controller->GetLocalPlayer() : nullptr;
	UWorld* World = Controller ? Controller->GetWorld() : nullptr;
	if (!LocalPlayer || !World || !World->Scene || !FMath::IsFinite(UV.X) || !FMath::IsFinite(UV.Y) ||
		UV.X < 0.0 || UV.X >= 1.0 || UV.Y < 0.0 || UV.Y >= 1.0)
	{
		FGaussianPointSplatPickResult Result;
		Result.Error = TEXT("A local player and viewport UV in [0,1) are required.");
		Finish(Result);
		return;
	}
	TWeakObjectPtr<UGaussianPointSplatPick> WeakThis(this);
	FSceneInterface* Scene = World->Scene;
	const int32 PlayerIndex = LocalPlayer->GetControllerId();
	const FVector2f RequestUV(UV);
	const float Threshold = CoverageThreshold;
	ENQUEUE_RENDER_COMMAND(GPSRequestPick)([Scene, PlayerIndex, RequestUV, Threshold, WeakThis](FRHICommandListImmediate&)
	{
		FGaussianPointSplatPicking::Request(Scene, PlayerIndex, RequestUV, Threshold,
			[WeakThis](const FGaussianPointSplatPickResult& Result)
			{
				if (WeakThis.IsValid()) WeakThis->Finish(Result);
			});
	});
}

void UGaussianPointSplatPick::Finish(const FGaussianPointSplatPickResult& Result)
{
	check(IsInGameThread());
	if (bFinished) return;
	bFinished = true;
	Completed.Broadcast(Result);
	SetReadyToDestroy();
}

