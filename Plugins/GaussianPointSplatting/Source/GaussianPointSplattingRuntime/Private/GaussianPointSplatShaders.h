#pragma once

#include "CoreMinimal.h"

class FRDGBuilder;
class FRDGTexture;
class FSceneView;
struct FScreenPassTexture;
struct FGaussianPointSplatProxyData;
struct FGPSRenderFrame;

class FGaussianPointSplatRenderer
{
public:
	static FScreenPassTexture AddRenderPass(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		FScreenPassTexture SceneColor,
		const FGaussianPointSplatProxyData& Data,
		FRDGTexture* SceneDepthTexture,
		FRDGTexture* FurthestHZBTexture,
		const FVector2f& HZBSize,
		const FVector2f& ViewportUVToHZBBufferUV,
		FGPSRenderFrame& Frame);
};
