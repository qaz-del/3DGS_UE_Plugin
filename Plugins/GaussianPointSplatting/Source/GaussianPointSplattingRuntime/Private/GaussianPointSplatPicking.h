#pragma once
#include "CoreMinimal.h"
#include "GaussianPointSplatFrame.h"
#include "GaussianPointSplatPick.h"

class FRDGBuilder;
class FSceneView;
class FSceneInterface;
struct FScreenPassTexture;

class FGaussianPointSplatPicking
{
public:
	// Render thread only. Completion is always delivered on the game thread.
	static void Request(FSceneInterface* Scene, int32 PlayerIndex, FVector2f UV, float MinCoverage,
		TFunction<void(const FGaussianPointSplatPickResult&)> Completion);
	static bool HasRequests();
	static void Poll();
	static void AddPasses(FRDGBuilder& GraphBuilder, const FSceneView& View, const FScreenPassTexture& SceneColor,
		const FGPSRenderFrame& Frame, const TArray<FGPSPickOwner>& Owners);
	static void Shutdown();
};
