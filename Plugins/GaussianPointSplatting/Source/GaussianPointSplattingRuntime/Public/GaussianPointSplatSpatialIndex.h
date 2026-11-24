#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GaussianPointSplatTypes.h"
#include "GaussianPointSplatSpatialIndex.generated.h"

class UGaussianPointSplatAsset;

USTRUCT()
struct FGaussianPointSplatSpatialSection
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UGaussianPointSplatAsset> SourceAsset = nullptr;

	UPROPERTY()
	int32 SourceRevision = 0;

	UPROPERTY()
	TArray<int32> OrderedIndices;

	UPROPERTY()
	TArray<FGaussianPointSplatBVHNode> Nodes;
};

UCLASS(BlueprintType)
class GAUSSIANPOINTSPLATTINGRUNTIME_API UGaussianPointSplatSpatialIndex : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting|Spatial Index")
	TObjectPtr<UGaussianPointSplatAsset> SourceAsset = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting|Spatial Index", meta = (ClampMin = "256", ClampMax = "65536"))
	int32 LeafSize = 8192;

	UPROPERTY(VisibleAnywhere, Category = "Gaussian Splatting|Spatial Index")
	int32 TotalIndexedGaussians = 0;

	UPROPERTY(VisibleAnywhere, Category = "Gaussian Splatting|Spatial Index")
	int32 BuiltLeafSize = 0;

	UPROPERTY()
	TArray<FGaussianPointSplatSpatialSection> Sections;

	UFUNCTION(CallInEditor, Category = "Gaussian Splatting|Spatial Index")
	void RefreshIndex();

	bool IsValidFor(const UGaussianPointSplatAsset* Asset) const;
};
