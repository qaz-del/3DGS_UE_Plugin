#include "GaussianPointSplatComponent.h"

#include "GaussianPointSplatAsset.h"
#include "GaussianPointSplatLog.h"
#include "GaussianPointSplatSceneProxy.h"

#if WITH_EDITOR
#include "GaussianPointSplatSpatialIndex.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#endif

UGaussianPointSplatComponent::UGaussianPointSplatComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	bUseAsOccluder = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void UGaussianPointSplatComponent::SetGaussianAsset(UGaussianPointSplatAsset* InAsset)
{
	Asset = InAsset;
	UE_LOG(
		LogGaussianPointSplatting,
		Verbose,
		TEXT("SetGaussianAsset: Component=%s Asset=%s NumGaussians=%d"),
		*GetPathName(),
		Asset ? *Asset->GetPathName() : TEXT("None"),
		Asset ? Asset->GetTotalGaussianCount() : 0);
	MarkRenderStateDirty();
	UpdateBounds();
}

FPrimitiveSceneProxy* UGaussianPointSplatComponent::CreateSceneProxy()
{
	if (!Asset)
	{
		UE_LOG(LogGaussianPointSplatting, Warning, TEXT("CreateSceneProxy skipped: Component=%s has no asset."), *GetPathName());
		return nullptr;
	}

	if (Asset->IsEmpty())
	{
		UE_LOG(LogGaussianPointSplatting, Warning, TEXT("CreateSceneProxy skipped: Asset=%s has no gaussians."), *Asset->GetPathName());
		return nullptr;
	}

	if (!bRenderWithRDGCompute)
	{
		UE_LOG(LogGaussianPointSplatting, Warning, TEXT("CreateSceneProxy skipped: bRenderWithRDGCompute is false on Component=%s."), *GetPathName());
		return nullptr;
	}

	UE_LOG(
		LogGaussianPointSplatting,
		Verbose,
		TEXT("CreateSceneProxy: Component=%s Asset=%s AssetGaussians=%d RootGaussians=%d ChildChunks=%d MaxRenderGaussians=%d PointRadius=%.2f GaussianScaleMul=%.4f SourceScale=%.4f SourceRotation=%s Bounds=%s"),
		*GetPathName(),
		*Asset->GetPathName(),
		Asset->GetTotalGaussianCount(),
		Asset->NumGaussians,
		Asset->GetChildChunkCount(),
		MaxRenderGaussians,
		PointRadiusPixels,
		GaussianScaleMultiplier,
		SourceUniformScale,
		*SourceRotation.ToString(),
		*Asset->GetCombinedLocalBounds().ToString());

	return new FGaussianPointSplatSceneProxy(this);
}

FBoxSphereBounds UGaussianPointSplatComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (Asset)
	{
		const FBox SourceBox = Asset->GetCombinedLocalBounds().GetBox();
		FBox TransformedSourceBox(EForceInit::ForceInit);
		const FQuat SourceQuat = SourceRotation.Quaternion();
		const float SafeSourceScale = FMath::Max(SourceUniformScale, 0.000001f);

		for (int32 CornerIndex = 0; CornerIndex < 8; ++CornerIndex)
		{
			const FVector Corner(
				(CornerIndex & 1) ? SourceBox.Max.X : SourceBox.Min.X,
				(CornerIndex & 2) ? SourceBox.Max.Y : SourceBox.Min.Y,
				(CornerIndex & 4) ? SourceBox.Max.Z : SourceBox.Min.Z);
			TransformedSourceBox += SourceQuat.RotateVector(Corner * SafeSourceScale) + SourceOffset;
		}

		if (TransformedSourceBox.IsValid)
		{
			return FBoxSphereBounds(TransformedSourceBox).TransformBy(LocalToWorld);
		}
	}

	return FBoxSphereBounds(FSphere(LocalToWorld.GetLocation(), 1.0));
}

#if WITH_EDITOR
void UGaussianPointSplatComponent::CreateOrRefreshSpatialIndex()
{
	if (!Asset || Asset->IsEmpty())
	{
		UE_LOG(LogGaussianPointSplatting, Warning, TEXT("Create spatial index requires a nonempty Gaussian asset on %s."), *GetPathName());
		return;
	}

	UGaussianPointSplatSpatialIndex* Index = SpatialIndex;
	if (!Index || Index->SourceAsset != Asset)
	{
		const FString IndexAssetName = Asset->GetName() + TEXT("_SpatialIndex");
		const FString PackagePath = FPackageName::GetLongPackagePath(Asset->GetOutermost()->GetName());
		const FString IndexPackageName = PackagePath / IndexAssetName;
		const FString IndexObjectPath = IndexPackageName + TEXT(".") + IndexAssetName;
		UObject* ExistingAsset = LoadObject<UObject>(nullptr, *IndexObjectPath);
		Index = Cast<UGaussianPointSplatSpatialIndex>(ExistingAsset);
		if (ExistingAsset && !Index)
		{
			UE_LOG(LogGaussianPointSplatting, Error, TEXT("Cannot create spatial index: %s is occupied by another asset type."), *IndexObjectPath);
			return;
		}
		if (!Index)
		{
			UPackage* IndexPackage = CreatePackage(*IndexPackageName);
			Index = NewObject<UGaussianPointSplatSpatialIndex>(IndexPackage, *IndexAssetName, RF_Public | RF_Standalone | RF_Transactional);
			FAssetRegistryModule::AssetCreated(Index);
		}
	}

	Modify();
	SpatialIndex = Index;
	Index->SourceAsset = Asset;
	Index->RefreshIndex();
	GetOutermost()->MarkPackageDirty();
	MarkRenderStateDirty();
}

void UGaussianPointSplatComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	MarkRenderStateDirty();
	UpdateBounds();
}
#endif
