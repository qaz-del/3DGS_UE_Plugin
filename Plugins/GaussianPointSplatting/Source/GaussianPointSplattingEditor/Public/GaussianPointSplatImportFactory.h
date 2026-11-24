#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "GaussianPointSplatImportFactory.generated.h"

UCLASS()
class GAUSSIANPOINTSPLATTINGEDITOR_API UGaussianPointSplatImportFactory : public UFactory
{
	GENERATED_BODY()

public:
	UGaussianPointSplatImportFactory();

	virtual UObject* FactoryCreateFile(
		UClass* InClass,
		UObject* InParent,
		FName InName,
		EObjectFlags Flags,
		const FString& Filename,
		const TCHAR* Parms,
		FFeedbackContext* Warn,
		bool& bOutOperationCanceled) override;
};
