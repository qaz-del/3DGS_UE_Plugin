#include "GaussianPointSplatActor.h"

#include "GaussianPointSplatComponent.h"

AGaussianPointSplatActor::AGaussianPointSplatActor()
{
	PrimaryActorTick.bCanEverTick = false;

	GaussianPointSplatComponent = CreateDefaultSubobject<UGaussianPointSplatComponent>(TEXT("GaussianPointSplatComponent"));
	RootComponent = GaussianPointSplatComponent;
}
