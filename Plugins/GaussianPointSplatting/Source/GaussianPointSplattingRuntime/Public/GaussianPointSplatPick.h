#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "GaussianPointSplatPick.generated.h"

class APlayerController;
class UGaussianPointSplatComponent;
class UGaussianPointSplatAsset;

USTRUCT(BlueprintType)
struct GAUSSIANPOINTSPLATTINGRUNTIME_API FGaussianPointSplatPickResult
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Gaussian Splatting") bool bHit = false;
	UPROPERTY(BlueprintReadOnly, Category="Gaussian Splatting") TObjectPtr<UGaussianPointSplatComponent> Component = nullptr;
	UPROPERTY(BlueprintReadOnly, Category="Gaussian Splatting") TObjectPtr<UGaussianPointSplatAsset> SourceAsset = nullptr;
	UPROPERTY(BlueprintReadOnly, Category="Gaussian Splatting") int32 SampleIndex = INDEX_NONE;
	UPROPERTY(BlueprintReadOnly, Category="Gaussian Splatting") int32 FlatSampleIndex = INDEX_NONE;
	UPROPERTY(BlueprintReadOnly, Category="Gaussian Splatting") float ViewDepthCm = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category="Gaussian Splatting") float Coverage = 0.0f;
	// Center of the selected Gaussian, not a mesh surface or physics hit.
	UPROPERTY(BlueprintReadOnly, Category="Gaussian Splatting") FVector GaussianCenter = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category="Gaussian Splatting") FString Error;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FGaussianPointSplatPicked, const FGaussianPointSplatPickResult&, Result);

// Local GPU screen query. Does not replicate or perform a collision trace.
UCLASS()
class GAUSSIANPOINTSPLATTINGRUNTIME_API UGaussianPointSplatPick : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintAssignable, Category="Gaussian Splatting") FGaussianPointSplatPicked Completed;
	// UV is normalized to this local player's constrained viewport (0..1).
	UFUNCTION(BlueprintCallable, Category="Gaussian Splatting", meta=(BlueprintInternalUseOnly="true", DisplayName="Pick Gaussian at Screen UV"))
	static UGaussianPointSplatPick* PickGaussianAtScreenUV(APlayerController* PlayerController, FVector2D ScreenUV, float MinCoverage = 0.25f);
	virtual void Activate() override;
private:
	void Finish(const FGaussianPointSplatPickResult& Result);
	UPROPERTY() TObjectPtr<APlayerController> Controller;
	FVector2D UV = FVector2D::ZeroVector;
	float CoverageThreshold = 0.25f;
	bool bFinished = false;
};
