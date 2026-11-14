#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"

class FRHICommandListImmediate;

class FDDGIRadianceCascadeRHIViewExtension final : public FSceneViewExtensionBase
{
public:
	explicit FDDGIRadianceCascadeRHIViewExtension(const FAutoRegister& AutoRegister);

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void PreRenderView_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneView& InView) override;

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;
};
