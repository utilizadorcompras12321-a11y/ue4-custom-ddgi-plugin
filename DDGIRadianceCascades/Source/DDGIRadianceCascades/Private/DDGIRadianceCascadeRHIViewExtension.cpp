#include "DDGIRadianceCascadeRHIViewExtension.h"

#include "DDGIMasterSubsystem.h"
#include "DDGIRadianceCascadeResources.h"

#include "GlobalShader.h"
#include "ShaderParameterUtils.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "SceneInterface.h"
#include "SceneView.h"

class FDDGIRadianceCascadeInitializeRHI : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FDDGIRadianceCascadeInitializeRHI, Global);

public:
	FDDGIRadianceCascadeInitializeRHI() {}

	explicit FDDGIRadianceCascadeInitializeRHI(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
		TotalRaysPerProbe.Bind(Initializer.ParameterMap, TEXT("TotalRaysPerProbe"));
		CascadeCount.Bind(Initializer.ParameterMap, TEXT("CascadeCount"));
		BaseRayCount.Bind(Initializer.ParameterMap, TEXT("BaseRayCount"));
		RaySplitFactor.Bind(Initializer.ParameterMap, TEXT("RaySplitFactor"));
		TextureWidth.Bind(Initializer.ParameterMap, TEXT("TextureWidth"));
		TotalTexels.Bind(Initializer.ParameterMap, TEXT("TotalTexels"));
		DispatchGroupsX.Bind(Initializer.ParameterMap, TEXT("DispatchGroupsX"));
		ProbeCount.Bind(Initializer.ParameterMap, TEXT("ProbeCount"));
		ProbePositionsTexture.Bind(Initializer.ParameterMap, TEXT("ProbePositionsTexture"));
		DirectionsUAV.Bind(Initializer.ParameterMap, TEXT("DirectionsUAV"));
		RadianceUAV.Bind(Initializer.ParameterMap, TEXT("RadianceUAV"));
		HitDistanceUAV.Bind(Initializer.ParameterMap, TEXT("HitDistanceUAV"));
		RayOriginsUAV.Bind(Initializer.ParameterMap, TEXT("RayOriginsUAV"));
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	virtual bool Serialize(FArchive& Ar) override
	{
		const bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		Ar << TotalRaysPerProbe;
		Ar << CascadeCount;
		Ar << BaseRayCount;
		Ar << RaySplitFactor;
		Ar << TextureWidth;
		Ar << TotalTexels;
		Ar << DispatchGroupsX;
		Ar << ProbeCount;
		Ar << ProbePositionsTexture;
		Ar << DirectionsUAV;
		Ar << RadianceUAV;
		Ar << HitDistanceUAV;
		Ar << RayOriginsUAV;
		return bShaderHasOutdatedParameters;
	}

	void SetParameters(
		FRHICommandList& RHICmdList,
		FShaderResourceViewRHIRef ProbePositionsSRV,
		FUnorderedAccessViewRHIRef DirectionsView,
		FUnorderedAccessViewRHIRef RadianceView,
		FUnorderedAccessViewRHIRef HitDistanceView,
		FUnorderedAccessViewRHIRef RayOriginsView,
		uint32 InTotalRaysPerProbe,
		uint32 InCascadeCount,
		uint32 InBaseRayCount,
		uint32 InRaySplitFactor,
		uint32 InProbeCount,
		uint32 InTotalTexels,
		uint32 InDispatchGroupsX)
	{
		FComputeShaderRHIParamRef ShaderRHI = GetComputeShader();
		const uint32 InTextureWidth = 4096;

		SetShaderValue(RHICmdList, ShaderRHI, TotalRaysPerProbe, InTotalRaysPerProbe);
		SetShaderValue(RHICmdList, ShaderRHI, CascadeCount, InCascadeCount);
		SetShaderValue(RHICmdList, ShaderRHI, BaseRayCount, InBaseRayCount);
		SetShaderValue(RHICmdList, ShaderRHI, RaySplitFactor, InRaySplitFactor);
		SetShaderValue(RHICmdList, ShaderRHI, TextureWidth, InTextureWidth);
		SetShaderValue(RHICmdList, ShaderRHI, TotalTexels, InTotalTexels);
		SetShaderValue(RHICmdList, ShaderRHI, DispatchGroupsX, InDispatchGroupsX);
		SetShaderValue(RHICmdList, ShaderRHI, ProbeCount, InProbeCount);

		if (ProbePositionsTexture.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, ProbePositionsTexture.GetBaseIndex(), ProbePositionsSRV);
		}
		if (DirectionsUAV.IsBound())
		{
			RHICmdList.SetUAVParameter(ShaderRHI, DirectionsUAV.GetBaseIndex(), DirectionsView);
		}
		if (RadianceUAV.IsBound())
		{
			RHICmdList.SetUAVParameter(ShaderRHI, RadianceUAV.GetBaseIndex(), RadianceView);
		}
		if (HitDistanceUAV.IsBound())
		{
			RHICmdList.SetUAVParameter(ShaderRHI, HitDistanceUAV.GetBaseIndex(), HitDistanceView);
		}
		if (RayOriginsUAV.IsBound())
		{
			RHICmdList.SetUAVParameter(ShaderRHI, RayOriginsUAV.GetBaseIndex(), RayOriginsView);
		}
	}

	void UnbindResources(FRHICommandList& RHICmdList)
	{
		FComputeShaderRHIParamRef ShaderRHI = GetComputeShader();
		if (ProbePositionsTexture.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, ProbePositionsTexture.GetBaseIndex(), FShaderResourceViewRHIParamRef());
		}
		if (DirectionsUAV.IsBound())
		{
			RHICmdList.SetUAVParameter(ShaderRHI, DirectionsUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
		}
		if (RadianceUAV.IsBound())
		{
			RHICmdList.SetUAVParameter(ShaderRHI, RadianceUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
		}
		if (HitDistanceUAV.IsBound())
		{
			RHICmdList.SetUAVParameter(ShaderRHI, HitDistanceUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
		}
		if (RayOriginsUAV.IsBound())
		{
			RHICmdList.SetUAVParameter(ShaderRHI, RayOriginsUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
		}
	}

private:
	FShaderParameter TotalRaysPerProbe;
	FShaderParameter CascadeCount;
	FShaderParameter BaseRayCount;
	FShaderParameter RaySplitFactor;
	FShaderParameter TextureWidth;
	FShaderParameter TotalTexels;
	FShaderParameter DispatchGroupsX;
	FShaderParameter ProbeCount;
	FShaderResourceParameter ProbePositionsTexture;
	FShaderResourceParameter DirectionsUAV;
	FShaderResourceParameter RadianceUAV;
	FShaderResourceParameter HitDistanceUAV;
	FShaderResourceParameter RayOriginsUAV;
};

IMPLEMENT_SHADER_TYPE(
	,
	FDDGIRadianceCascadeInitializeRHI,
	TEXT("/Plugin/DDGIRadianceCascades/Private/RadianceCascades/RadianceCascadeInitializeCS.usf"),
	TEXT("RadianceCascadeInitializeCS"),
	SF_Compute);

FDDGIRadianceCascadeRHIViewExtension::FDDGIRadianceCascadeRHIViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
}

bool FDDGIRadianceCascadeRHIViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	return true;
}

void FDDGIRadianceCascadeRHIViewExtension::PreRenderView_RenderThread(
	FRHICommandListImmediate& RHICmdList,
	FSceneView& InView)
{
	check(IsInRenderingThread());

	if (!InView.Family || !InView.Family->Scene)
	{
		return;
	}

	const FSceneInterface* Scene = InView.Family->Scene;
	FDDGIMasterConfig Config;
	if (!DDGIGetMasterConfig_RenderThread(Scene, Config))
	{
		return;
	}

	FTexture2DRHIRef DirectionsTexture;
	FTexture2DRHIRef RadianceTexture;
	FTexture2DRHIRef HitDistanceTexture;
	FTexture2DRHIRef RayOriginsTexture;
	FTexture2DRHIRef ProbePositionsTexture;
	uint32 ProbeCount = 0;

	if (!DDGIGetRadianceCascadeInitializationRHIResources_RenderThread(
		Scene,
		DirectionsTexture,
		RadianceTexture,
		HitDistanceTexture,
		RayOriginsTexture,
		ProbePositionsTexture,
		ProbeCount))
	{
		return;
	}

	const uint32 TotalRaysPerProbe = static_cast<uint32>(FMath::Max(1, Config.TotalRaysPerProbe));
	const uint32 TotalTexels = ProbeCount * TotalRaysPerProbe;
	if (TotalTexels == 0)
	{
		return;
	}

	FShaderResourceViewRHIRef ProbePositionsSRV = RHICreateShaderResourceView(ProbePositionsTexture, 0);
	FUnorderedAccessViewRHIRef DirectionsUAV = RHICreateUnorderedAccessView(DirectionsTexture);
	FUnorderedAccessViewRHIRef RadianceUAV = RHICreateUnorderedAccessView(RadianceTexture);
	FUnorderedAccessViewRHIRef HitDistanceUAV = RHICreateUnorderedAccessView(HitDistanceTexture);
	FUnorderedAccessViewRHIRef RayOriginsUAV = RHICreateUnorderedAccessView(RayOriginsTexture);

	if (!ProbePositionsSRV.IsValid() || !DirectionsUAV.IsValid() || !RadianceUAV.IsValid() ||
		!HitDistanceUAV.IsValid() || !RayOriginsUAV.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to create RHI SRV/UAV views for initialization."));
		return;
	}

	const uint32 TotalGroups = FMath::DivideAndRoundUp(TotalTexels, 64u);
	const uint32 GroupsX = FMath::Min(TotalGroups, 65535u);
	const uint32 GroupsY = FMath::DivideAndRoundUp(TotalGroups, GroupsX);

	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, ProbePositionsTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, DirectionsTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, RadianceTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, HitDistanceTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, RayOriginsTexture);

	TShaderMapRef<FDDGIRadianceCascadeInitializeRHI> ComputeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
	RHICmdList.SetComputeShader(ComputeShader->GetComputeShader());
	ComputeShader->SetParameters(
		RHICmdList,
		ProbePositionsSRV,
		DirectionsUAV,
		RadianceUAV,
		HitDistanceUAV,
		RayOriginsUAV,
		TotalRaysPerProbe,
		static_cast<uint32>(Config.RadianceCascades.GetSafeCascadeCount()),
		static_cast<uint32>(FMath::Clamp(Config.RadianceCascades.BaseRayCount, 1, 64)),
		static_cast<uint32>(FMath::Clamp(Config.RadianceCascades.RaySplitFactor, 1, 4)),
		ProbeCount,
		TotalTexels,
		GroupsX);

	DispatchComputeShader(RHICmdList, *ComputeShader, GroupsX, GroupsY, 1);
	ComputeShader->UnbindResources(RHICmdList);

	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, DirectionsTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, RadianceTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, HitDistanceTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, RayOriginsTexture);

	DDGIMarkRadianceCascadeInitializationScheduled_RenderThread(Scene);
}
