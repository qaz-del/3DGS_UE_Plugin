#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GaussianPointSplatActor.generated.h"

class UGaussianPointSplatComponent;

UCLASS(BlueprintType, Blueprintable)
class GAUSSIANPOINTSPLATTINGRUNTIME_API AGaussianPointSplatActor : public AActor
{
	GENERATED_BODY()

public:
	AGaussianPointSplatActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	TObjectPtr<UGaussianPointSplatComponent> GaussianPointSplatComponent = nullptr;
};
