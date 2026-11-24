#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "GaussianPointSplatSpatialIndexFactory.generated.h"

UCLASS()
class GAUSSIANPOINTSPLATTINGEDITOR_API UGaussianPointSplatSpatialIndexFactory : public UFactory
{
	GENERATED_BODY()

public:
	UGaussianPointSplatSpatialIndexFactory();
	virtual UObject* FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags,
		UObject* Context, FFeedbackContext* Warn) override;
};
