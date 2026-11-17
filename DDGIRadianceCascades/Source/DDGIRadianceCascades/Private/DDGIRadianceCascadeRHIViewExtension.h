#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"

class FRHICommandListImmediate;
class FScene;
class FViewInfo;
class FRDGBuilder;
class FGlobalIlluminationExperimentalPluginResources;

class FDDGIRadianceCascadeRHIViewExtension final : public FSceneViewExtensionBase
{
public:
	explicit FDDGIRadianceCascadeRHIViewExtension(const FAutoRegister& AutoRegister);

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void PreRenderViewFamily_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneViewFamily& InViewFamily) override {}
	virtual void PreRenderView_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneView& InView) override;
	static void RenderDiffuseIndirect(
		const FScene& Scene,
		const FViewInfo& View,
		FRDGBuilder& GraphBuilder,
		FGlobalIlluminationExperimentalPluginResources& Resources);

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;
};
