#pragma once

#include "CoreMinimal.h"
#include "GaussianPointSplatTypes.h"
#include "GaussianPointSplatAsset.generated.h"

UCLASS(BlueprintType)
class GAUSSIANPOINTSPLATTINGRUNTIME_API UGaussianPointSplatAsset : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	int32 NumGaussians = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	FBoxSphereBounds LocalBounds = FBoxSphereBounds(EForceInit::ForceInit);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	int32 SHDegree = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	FString SourceFile;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	int32 DataRevision = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	int32 NumChunks = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting|BVH")
	int32 NumBVHNodes = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting|BVH")
	int32 BVHRootNode = INDEX_NONE;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting|BVH")
	int32 BVHLeafChunkSize = 8192;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting|BVH")
	bool bSpatiallySortedForBVH = false;

	UPROPERTY()
	TArray<FGaussianPointSplatSample> Gaussians;

	UPROPERTY()
	TArray<FGaussianPointSplatChunk> Chunks;

	UPROPERTY()
	TArray<FGaussianPointSplatBVHNode> BVHNodes;

	UPROPERTY()
	TArray<TObjectPtr<UGaussianPointSplatAsset>> ChildChunks;

	UFUNCTION(BlueprintPure, Category = "Gaussian Splatting")
	int32 GetGaussianCount() const;

	UFUNCTION(BlueprintPure, Category = "Gaussian Splatting")
	int32 GetTotalGaussianCount() const;

	UFUNCTION(BlueprintPure, Category = "Gaussian Splatting")
	int32 GetChildChunkCount() const { return ChildChunks.Num(); }

	UFUNCTION(BlueprintPure, Category = "Gaussian Splatting")
	bool IsEmpty() const { return GetTotalGaussianCount() <= 0; }

	FBoxSphereBounds GetCombinedLocalBounds() const;

	void SetGaussianData(TArray<FGaussianPointSplatSample>&& InGaussians, const FString& InSourceFile, int32 InSHDegree);
	void RebuildBoundsAndChunks(int32 ChunkSize = 8192);
};
