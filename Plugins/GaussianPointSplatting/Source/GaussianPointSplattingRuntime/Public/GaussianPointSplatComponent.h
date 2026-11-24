#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "GaussianPointSplatComponent.generated.h"

class UGaussianPointSplatAsset;
class UGaussianPointSplatSpatialIndex;

UCLASS(ClassGroup = (Rendering), meta = (BlueprintSpawnableComponent))
class GAUSSIANPOINTSPLATTINGRUNTIME_API UGaussianPointSplatComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UGaussianPointSplatComponent();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	TObjectPtr<UGaussianPointSplatAsset> Asset = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting|Spatial Index")
	TObjectPtr<UGaussianPointSplatSpatialIndex> SpatialIndex = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Source Transform", meta = (ClampMin = "0.000001", ClampMax = "1000000.0"))
	float SourceUniformScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Source Transform")
	FRotator SourceRotation = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Source Transform")
	FVector SourceOffset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Quality", meta = (ClampMin = "1.0", ClampMax = "64.0"))
	float PointRadiusPixels = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Quality", meta = (ClampMin = "0.0001", ClampMax = "100000.0"))
	float GaussianScaleMultiplier = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Quality", meta = (ClampMin = "1.0", ClampMax = "512.0"))
	float MaxSplatRadiusPixels = 24.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Quality", meta = (ClampMin = "0.0", ClampMax = "8.0"))
	float MinSplatRadiusPixels = 0.75f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Quality", meta = (ClampMin = "1", ClampMax = "100000000", UIMin = "1", UIMax = "50000000"))
	int32 MaxRenderGaussians = 15000000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Debug")
	bool bRenderWithRDGCompute = true;

	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting")
	void SetGaussianAsset(UGaussianPointSplatAsset* InAsset);

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual int32 GetNumMaterials() const override { return 0; }

#if WITH_EDITOR
	UFUNCTION(CallInEditor, Category = "Gaussian Splatting|Spatial Index", meta = (DisplayName = "Create or Refresh Spatial Index"))
	void CreateOrRefreshSpatialIndex();

	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
