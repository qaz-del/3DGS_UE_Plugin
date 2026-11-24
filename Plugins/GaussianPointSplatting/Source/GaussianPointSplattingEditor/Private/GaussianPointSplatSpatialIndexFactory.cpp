#include "GaussianPointSplatSpatialIndexFactory.h"

#include "GaussianPointSplatSpatialIndex.h"

UGaussianPointSplatSpatialIndexFactory::UGaussianPointSplatSpatialIndexFactory()
{
	SupportedClass = UGaussianPointSplatSpatialIndex::StaticClass();
	bCreateNew = true;
	bEditAfterNew = true;
}

UObject* UGaussianPointSplatSpatialIndexFactory::FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName,
	EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn)
{
	return NewObject<UGaussianPointSplatSpatialIndex>(InParent, InClass, InName, Flags);
}
