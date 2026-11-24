#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"

class FRDGBuilder;
class FSceneView;
struct FScreenPassTexture;
struct FGaussianPointSplatProxyData;
struct FGPSRenderFrame;

class FGaussianPointSplatPaperRenderer
{
public:
	static bool IsSupported(const FSceneView& View);

	static void AddPasses(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FScreenPassTexture& SceneColor,
		FGPSRenderFrame& Frame,
		FRDGTextureRef SceneDepthTexture,
		const FGaussianPointSplatProxyData& Data,
		FRDGBufferRef GeometryBuffer,
		FRDGBufferRef ColorBuffer,
		FRDGBufferRef OpacityBuffer,
		FRDGBufferRef VisibleIndexBuffer,
		uint32 NumGaussians,
		bool bUseVisibleIndices,
		FRDGBufferRef SourceIDBuffer);
};
