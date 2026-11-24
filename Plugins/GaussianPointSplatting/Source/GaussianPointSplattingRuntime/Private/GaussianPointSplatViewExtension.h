#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"

class FGaussianPointSplatViewExtension final : public FSceneViewExtensionBase
{
public:
	explicit FGaussianPointSplatViewExtension(const FAutoRegister& AutoRegister);

	virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;
	FScreenPassTexture PostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& InView, const FPostProcessMaterialInputs& Inputs);

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;
};
