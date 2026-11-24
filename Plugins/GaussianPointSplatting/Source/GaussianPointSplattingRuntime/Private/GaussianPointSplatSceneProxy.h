#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "RenderGraphResources.h"
#include "GaussianPointSplatFrame.h"

class UGaussianPointSplatComponent;

struct FGaussianPointSplatRenderNode
{
	FBoxSphereBounds Bounds = FBoxSphereBounds(EForceInit::ForceInit);
	FBoxSphereBounds LocalBounds = FBoxSphereBounds(EForceInit::ForceInit);
	int32 FirstGaussian = 0;
	int32 NumGaussians = 0;
	int32 LeftChild = INDEX_NONE;
	int32 RightChild = INDEX_NONE;
};

struct FGaussianPointSplatProxyData
{
	TArray<FUintVector4> PackedGeometry;
	TArray<uint32> PackedColors;
	TArray<uint32> PackedOpacityWords;
	TArray<uint32> SourceIDs;
	mutable TRefCountPtr<FRDGPooledBuffer> SourceIDBuffer;
	FGPSPickOwner PickOwner;
	mutable TRefCountPtr<FRDGPooledBuffer> GeometryBuffer;
	mutable TRefCountPtr<FRDGPooledBuffer> ColorBuffer;
	mutable TRefCountPtr<FRDGPooledBuffer> OpacityBuffer;
	TArray<FGaussianPointSplatRenderNode> Nodes;
	TArray<int32> RootNodes;
	int32 NumGaussians = 0;
	FVector4f PositionMin = FVector4f::Zero();
	FVector4f PositionExtent = FVector4f(1.0f);
	float ScaleLogMin = 0.0f;
	float ScaleLogExtent = 1.0f;
	FMatrix SourceToLocal = FMatrix::Identity;
	FMatrix44f LocalToWorld = FMatrix44f::Identity;
	FBoxSphereBounds Bounds = FBoxSphereBounds(EForceInit::ForceInit);
	float PointRadiusPixels = 2.0f;
	float GaussianScaleMultiplier = 1.0f;
	float MaxSplatRadiusPixels = 24.0f;
	float MinSplatRadiusPixels = 0.75f;
};

class FGaussianPointSplatSceneProxy final : public FPrimitiveSceneProxy
{
public:
	explicit FGaussianPointSplatSceneProxy(const UGaussianPointSplatComponent* Component);
	virtual ~FGaussianPointSplatSceneProxy() override;
	virtual void OnTransformChanged(FRHICommandListBase& RHICmdList) override;

	virtual SIZE_T GetTypeHash() const override;
	virtual uint32 GetMemoryFootprint() const override;
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;
	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override;

	const FGaussianPointSplatProxyData& GetData() const { return Data; }

	static int32 GetProxyCount();
	static void ForEachProxy(TFunctionRef<void(const FGaussianPointSplatSceneProxy&)> Callback);

private:
	FGaussianPointSplatProxyData Data;
};
