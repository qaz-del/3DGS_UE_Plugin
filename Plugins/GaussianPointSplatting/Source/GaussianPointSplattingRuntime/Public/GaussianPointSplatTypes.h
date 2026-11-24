#pragma once

#include "CoreMinimal.h"
#include "GaussianPointSplatTypes.generated.h"

USTRUCT(BlueprintType)
struct FGaussianPointSplatSample
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	FVector3f Position = FVector3f::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	FVector3f Scale = FVector3f(1.0f, 1.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	FVector4f Rotation = FVector4f(0.0f, 0.0f, 0.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	FLinearColor Color = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Opacity = 1.0f;
};

USTRUCT(BlueprintType)
struct FGaussianPointSplatChunk
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	int32 FirstGaussian = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	int32 NumGaussians = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	FBox LocalBounds = FBox(EForceInit::ForceInit);
};

USTRUCT(BlueprintType)
struct FGaussianPointSplatBVHNode
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	FBox LocalBounds = FBox(EForceInit::ForceInit);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	float MaxGaussianScale = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	int32 FirstGaussian = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	int32 NumGaussians = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	int32 FirstChunk = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	int32 NumChunks = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	int32 LeftChild = INDEX_NONE;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	int32 RightChild = INDEX_NONE;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	bool bLeaf = false;
};
