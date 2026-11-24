#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"

class UGaussianPointSplatComponent;
class UGaussianPointSplatAsset;

struct FGPSPickSource
{
	TWeakObjectPtr<UGaussianPointSplatAsset> Asset;
	int32 Revision = 0;
	uint32 First = 0;
	uint32 Count = 0;
};

struct FGPSPickOwner
{
	uint32 ComponentID = 0;
	FMatrix SourceToWorld = FMatrix::Identity;
	TWeakObjectPtr<UGaussianPointSplatComponent> Component;
	TWeakObjectPtr<UGaussianPointSplatAsset> RootAsset;
	int32 RootRevision = 0;
	TArray<FGPSPickSource> Sources;
};

// One slice per visible component. Both renderers write the same layer contract.
// Color is premultiplied; Hit is {positive view-Z bits, original flat asset ID}.
struct FGPSRenderFrame
{
	FRDGTextureRef LayerColor = nullptr;
	FRDGTextureRef LayerHit = nullptr;
	FRDGTextureRef Depth = nullptr;
	FRDGTextureRef ID = nullptr;
	FRDGTextureRef Coverage = nullptr;
	FIntRect SceneDepthViewRect;
	FIntPoint SampleExtent = FIntPoint::ZeroValue;
	uint32 Supersampling = 1;
	uint32 LayerIndex = 0;
	uint32 LayerCount = 0;
	bool bUseSceneDepth = false;
	float SceneDepthBiasCm = 2.0f;
};
