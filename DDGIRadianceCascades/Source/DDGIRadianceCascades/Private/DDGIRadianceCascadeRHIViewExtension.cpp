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


class FDDGIRadianceCascadeTraceRHI : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FDDGIRadianceCascadeTraceRHI, Global);

public:
	FDDGIRadianceCascadeTraceRHI() {}

	explicit FDDGIRadianceCascadeTraceRHI(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
		TextureWidth.Bind(Initializer.ParameterMap, TEXT("TextureWidth"));
		TotalTexels.Bind(Initializer.ParameterMap, TEXT("TotalTexels"));
		DispatchGroupsX.Bind(Initializer.ParameterMap, TEXT("DispatchGroupsX"));
		ProbeCount.Bind(Initializer.ParameterMap, TEXT("ProbeCount"));
		TotalRaysPerProbe.Bind(Initializer.ParameterMap, TEXT("TotalRaysPerProbe"));
		NodeCount.Bind(Initializer.ParameterMap, TEXT("NodeCount"));
		TriangleCount.Bind(Initializer.ParameterMap, TEXT("TriangleCount"));
		MaxTraceDistance.Bind(Initializer.ParameterMap, TEXT("MaxTraceDistance"));
		StaticBackgroundColor.Bind(Initializer.ParameterMap, TEXT("StaticBackgroundColor"));
		StaticBackgroundIntensity.Bind(Initializer.ParameterMap, TEXT("StaticBackgroundIntensity"));
		GIIntensity.Bind(Initializer.ParameterMap, TEXT("GIIntensity"));
		ProbeGridOrigin.Bind(Initializer.ParameterMap, TEXT("ProbeGridOrigin"));
		BackgroundTransitionStart.Bind(Initializer.ParameterMap, TEXT("BackgroundTransitionStart"));
		BackgroundTransitionEnd.Bind(Initializer.ParameterMap, TEXT("BackgroundTransitionEnd"));
		bEnableStaticBackground.Bind(Initializer.ParameterMap, TEXT("bEnableStaticBackground"));

		DirectionalLightDirection.Bind(Initializer.ParameterMap, TEXT("DirectionalLightDirection"));
		DirectionalLightColor.Bind(Initializer.ParameterMap, TEXT("DirectionalLightColor"));
		DirectionalLightIntensity.Bind(Initializer.ParameterMap, TEXT("DirectionalLightIntensity"));
		bHasDirectionalLight.Bind(Initializer.ParameterMap, TEXT("bHasDirectionalLight"));
		DirectionsTexture.Bind(Initializer.ParameterMap, TEXT("DirectionsTexture"));
		RayOriginsTexture.Bind(Initializer.ParameterMap, TEXT("RayOriginsTexture"));
		BVHNodes.Bind(Initializer.ParameterMap, TEXT("BVHNodes"));
		BVHTriangles.Bind(Initializer.ParameterMap, TEXT("BVHTriangles"));
		RadianceUAV.Bind(Initializer.ParameterMap, TEXT("RadianceUAV"));
		HitDistanceUAV.Bind(Initializer.ParameterMap, TEXT("HitDistanceUAV"));
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	virtual bool Serialize(FArchive& Ar) override
	{
		const bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		Ar << TextureWidth;
		Ar << TotalTexels;
		Ar << DispatchGroupsX;
		Ar << ProbeCount;
		Ar << TotalRaysPerProbe;
		Ar << NodeCount;
		Ar << TriangleCount;
		Ar << MaxTraceDistance;
		Ar << StaticBackgroundColor;
		Ar << StaticBackgroundIntensity;
		Ar << GIIntensity;
		Ar << ProbeGridOrigin;
		Ar << BackgroundTransitionStart;
		Ar << BackgroundTransitionEnd;
		Ar << bEnableStaticBackground;

		Ar << DirectionalLightDirection;
		Ar << DirectionalLightColor;
		Ar << DirectionalLightIntensity;
		Ar << bHasDirectionalLight;
		Ar << DirectionsTexture;
		Ar << RayOriginsTexture;
		Ar << BVHNodes;
		Ar << BVHTriangles;
		Ar << RadianceUAV;
		Ar << HitDistanceUAV;
		return bShaderHasOutdatedParameters;
	}

	void SetParameters(
		FRHICommandList& RHICmdList,
		FShaderResourceViewRHIRef DirectionsSRV,
		FShaderResourceViewRHIRef OriginsSRV,
		FShaderResourceViewRHIRef NodesSRV,
		FShaderResourceViewRHIRef TrianglesSRV,
		FUnorderedAccessViewRHIRef RadianceView,
		FUnorderedAccessViewRHIRef HitDistanceView,
		uint32 InTotalTexels,
		uint32 InDispatchGroupsX,
		uint32 InProbeCount,
		uint32 InTotalRaysPerProbe,
		uint32 InNodeCount,
		uint32 InTriangleCount,
		float InMaxTraceDistance,
		const FVector4& InStaticBackgroundColor,
		float InStaticBackgroundIntensity,
		float InGIIntensity,
		const FVector4& InProbeGridOrigin,
		float InBackgroundTransitionStart,
		float InBackgroundTransitionEnd,
		uint32 bInEnableStaticBackground,
		const FVector4& InDirectionalLightDirection,
		const FVector4& InDirectionalLightColor,
		float InDirectionalLightIntensity,
		uint32 bInHasDirectionalLight)
	{
		FComputeShaderRHIParamRef ShaderRHI = GetComputeShader();
		const uint32 InTextureWidth = 4096;
		SetShaderValue(RHICmdList, ShaderRHI, TextureWidth, InTextureWidth);
		SetShaderValue(RHICmdList, ShaderRHI, TotalTexels, InTotalTexels);
		SetShaderValue(RHICmdList, ShaderRHI, DispatchGroupsX, InDispatchGroupsX);
		SetShaderValue(RHICmdList, ShaderRHI, ProbeCount, InProbeCount);
		SetShaderValue(RHICmdList, ShaderRHI, TotalRaysPerProbe, InTotalRaysPerProbe);
		SetShaderValue(RHICmdList, ShaderRHI, NodeCount, InNodeCount);
		SetShaderValue(RHICmdList, ShaderRHI, TriangleCount, InTriangleCount);
		SetShaderValue(RHICmdList, ShaderRHI, MaxTraceDistance, InMaxTraceDistance);
		SetShaderValue(RHICmdList, ShaderRHI, StaticBackgroundColor, InStaticBackgroundColor);
		SetShaderValue(RHICmdList, ShaderRHI, StaticBackgroundIntensity, InStaticBackgroundIntensity);
		SetShaderValue(RHICmdList, ShaderRHI, GIIntensity, InGIIntensity);
		SetShaderValue(RHICmdList, ShaderRHI, ProbeGridOrigin, InProbeGridOrigin);
		SetShaderValue(RHICmdList, ShaderRHI, BackgroundTransitionStart, InBackgroundTransitionStart);
		SetShaderValue(RHICmdList, ShaderRHI, BackgroundTransitionEnd, InBackgroundTransitionEnd);
		SetShaderValue(RHICmdList, ShaderRHI, bEnableStaticBackground, bInEnableStaticBackground);

		SetShaderValue(RHICmdList, ShaderRHI, DirectionalLightDirection, InDirectionalLightDirection);
		SetShaderValue(RHICmdList, ShaderRHI, DirectionalLightColor, InDirectionalLightColor);
		SetShaderValue(RHICmdList, ShaderRHI, DirectionalLightIntensity, InDirectionalLightIntensity);
		SetShaderValue(RHICmdList, ShaderRHI, bHasDirectionalLight, bInHasDirectionalLight);

		if (DirectionsTexture.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, DirectionsTexture.GetBaseIndex(), DirectionsSRV);
		if (RayOriginsTexture.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, RayOriginsTexture.GetBaseIndex(), OriginsSRV);
		if (BVHNodes.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, BVHNodes.GetBaseIndex(), NodesSRV);
		if (BVHTriangles.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, BVHTriangles.GetBaseIndex(), TrianglesSRV);
		if (RadianceUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, RadianceUAV.GetBaseIndex(), RadianceView);
		if (HitDistanceUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, HitDistanceUAV.GetBaseIndex(), HitDistanceView);
	}

	void UnbindResources(FRHICommandList& RHICmdList)
	{
		FComputeShaderRHIParamRef ShaderRHI = GetComputeShader();
		if (DirectionsTexture.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, DirectionsTexture.GetBaseIndex(), FShaderResourceViewRHIParamRef());
		if (RayOriginsTexture.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, RayOriginsTexture.GetBaseIndex(), FShaderResourceViewRHIParamRef());
		if (BVHNodes.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, BVHNodes.GetBaseIndex(), FShaderResourceViewRHIParamRef());
		if (BVHTriangles.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, BVHTriangles.GetBaseIndex(), FShaderResourceViewRHIParamRef());
		if (RadianceUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, RadianceUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
		if (HitDistanceUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, HitDistanceUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
	}

private:
	FShaderParameter TextureWidth;
	FShaderParameter TotalTexels;
	FShaderParameter DispatchGroupsX;
	FShaderParameter ProbeCount;
	FShaderParameter TotalRaysPerProbe;
	FShaderParameter NodeCount;
	FShaderParameter TriangleCount;
	FShaderParameter MaxTraceDistance;
	FShaderParameter StaticBackgroundColor;
	FShaderParameter StaticBackgroundIntensity;
	FShaderParameter GIIntensity;
	FShaderParameter ProbeGridOrigin;
	FShaderParameter BackgroundTransitionStart;
	FShaderParameter BackgroundTransitionEnd;
	FShaderParameter bEnableStaticBackground;

	FShaderParameter DirectionalLightDirection;
	FShaderParameter DirectionalLightColor;
	FShaderParameter DirectionalLightIntensity;
	FShaderParameter bHasDirectionalLight;
	FShaderResourceParameter DirectionsTexture;
	FShaderResourceParameter RayOriginsTexture;
	FShaderResourceParameter BVHNodes;
	FShaderResourceParameter BVHTriangles;
	FShaderResourceParameter RadianceUAV;
	FShaderResourceParameter HitDistanceUAV;
};

IMPLEMENT_SHADER_TYPE(
	,
	FDDGIRadianceCascadeTraceRHI,
	TEXT("/Plugin/DDGIRadianceCascades/Private/RadianceCascades/RadianceCascadeTraceCS.usf"),
	TEXT("RadianceCascadeTraceCS"),
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
	if (!DDGIGetMasterConfig_RenderThread(Scene, Config) || !Config.bEnableDynamicGI)
	{
		return;
	}

	FTexture2DRHIRef DirectionsTexture;
	FTexture2DRHIRef RadianceTexture;
	FTexture2DRHIRef HitDistanceTexture;
	FTexture2DRHIRef RayOriginsTexture;
	uint32 ProbeCount = 0;
	uint32 TotalRaysPerProbe = 0;
	if (!DDGIGetRadianceCascadeTracingRHIResources_RenderThread(
		Scene, DirectionsTexture, RadianceTexture, HitDistanceTexture, RayOriginsTexture,
		ProbeCount, TotalRaysPerProbe))
	{
		return;
	}

	const uint32 TotalTexels = ProbeCount * TotalRaysPerProbe;
	if (TotalTexels == 0)
	{
		return;
	}

	const uint32 TotalGroups = FMath::DivideAndRoundUp(TotalTexels, 64u);
	const uint32 GroupsX = FMath::Min(TotalGroups, 65535u);
	const uint32 GroupsY = FMath::DivideAndRoundUp(TotalGroups, GroupsX);

	// Initialize ray directions and origins only when the Master Actor publishes
	// new probe positions or the GPU storage is recreated.
	FTexture2DRHIRef InitProbePositions;
	uint32 InitProbeCount = 0;
	if (DDGIGetRadianceCascadeInitializationRHIResources_RenderThread(
		Scene, DirectionsTexture, RadianceTexture, HitDistanceTexture, RayOriginsTexture,
		InitProbePositions, InitProbeCount))
	{
		FShaderResourceViewRHIRef ProbePositionsSRV = RHICreateShaderResourceView(InitProbePositions, 0);
		FUnorderedAccessViewRHIRef DirectionsUAV = RHICreateUnorderedAccessView(DirectionsTexture);
		FUnorderedAccessViewRHIRef RadianceUAV = RHICreateUnorderedAccessView(RadianceTexture);
		FUnorderedAccessViewRHIRef HitDistanceUAV = RHICreateUnorderedAccessView(HitDistanceTexture);
		FUnorderedAccessViewRHIRef RayOriginsUAV = RHICreateUnorderedAccessView(RayOriginsTexture);

		if (ProbePositionsSRV.IsValid() && DirectionsUAV.IsValid() && RadianceUAV.IsValid() &&
			HitDistanceUAV.IsValid() && RayOriginsUAV.IsValid())
		{
			RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, InitProbePositions);
			RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, DirectionsTexture);
			RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, RadianceTexture);
			RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, HitDistanceTexture);
			RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, RayOriginsTexture);

			TShaderMapRef<FDDGIRadianceCascadeInitializeRHI> InitializeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
			RHICmdList.SetComputeShader(InitializeShader->GetComputeShader());
			InitializeShader->SetParameters(
				RHICmdList, ProbePositionsSRV, DirectionsUAV, RadianceUAV, HitDistanceUAV, RayOriginsUAV,
				TotalRaysPerProbe,
				static_cast<uint32>(Config.RadianceCascades.GetSafeCascadeCount()),
				static_cast<uint32>(FMath::Clamp(Config.RadianceCascades.BaseRayCount, 1, 64)),
				static_cast<uint32>(FMath::Clamp(Config.RadianceCascades.RaySplitFactor, 1, 4)),
				InitProbeCount, TotalTexels, GroupsX);
			DispatchComputeShader(RHICmdList, *InitializeShader, GroupsX, GroupsY, 1);
			InitializeShader->UnbindResources(RHICmdList);

			RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, DirectionsTexture);
			RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, RadianceTexture);
			RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, HitDistanceTexture);
			RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, RayOriginsTexture);
			DDGIMarkRadianceCascadeInitializationScheduled_RenderThread(Scene);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to create initialization SRV/UAV views."));
			return;
		}
	}

	FShaderResourceViewRHIRef NodesSRV;
	FShaderResourceViewRHIRef TrianglesSRV;
	uint32 NodeCount = 0;
	uint32 TriangleCount = 0;
	if (!DDGIGetSoftwareBVHResources_RenderThread(Scene, NodesSRV, TrianglesSRV, NodeCount, TriangleCount))
	{
		// No supported static geometry was captured; keep miss distances from initialization.
		return;
	}

	FShaderResourceViewRHIRef DirectionsSRV = RHICreateShaderResourceView(DirectionsTexture, 0);
	FShaderResourceViewRHIRef OriginsSRV = RHICreateShaderResourceView(RayOriginsTexture, 0);
	FUnorderedAccessViewRHIRef RadianceUAV = RHICreateUnorderedAccessView(RadianceTexture);
	FUnorderedAccessViewRHIRef HitDistanceUAV = RHICreateUnorderedAccessView(HitDistanceTexture);
	if (!DirectionsSRV.IsValid() || !OriginsSRV.IsValid() || !RadianceUAV.IsValid() || !HitDistanceUAV.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to create tracing SRV/UAV views."));
		return;
	}

	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, DirectionsTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, RayOriginsTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, RadianceTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, HitDistanceTexture);

	TShaderMapRef<FDDGIRadianceCascadeTraceRHI> TraceShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
	RHICmdList.SetComputeShader(TraceShader->GetComputeShader());
	TraceShader->SetParameters(
		RHICmdList, DirectionsSRV, OriginsSRV, NodesSRV, TrianglesSRV,
		RadianceUAV, HitDistanceUAV,
		TotalTexels, GroupsX, ProbeCount, TotalRaysPerProbe,
		NodeCount, TriangleCount, Config.RadianceCascades.MaxTraceDistance,
		FVector4(Config.StaticBackgroundColor.R, Config.StaticBackgroundColor.G, Config.StaticBackgroundColor.B, Config.StaticBackgroundColor.A),
		Config.StaticBackgroundIntensity, Config.GIIntensity,
		FVector4(Config.ProbeGridOrigin.X, Config.ProbeGridOrigin.Y, Config.ProbeGridOrigin.Z, 0.0f),
		Config.BackgroundTransitionStart, Config.BackgroundTransitionEnd,
		Config.bEnableStaticBackground ? 1u : 0u,
		FVector4(Config.DirectionalLightDirection.X, Config.DirectionalLightDirection.Y, Config.DirectionalLightDirection.Z, 0.0f),
		FVector4(Config.DirectionalLightColor.R, Config.DirectionalLightColor.G, Config.DirectionalLightColor.B, 0.0f),
		Config.DirectionalLightIntensity,
		Config.bHasDirectionalLight ? 1u : 0u);
	DispatchComputeShader(RHICmdList, *TraceShader, GroupsX, GroupsY, 1);
	TraceShader->UnbindResources(RHICmdList);

	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, RadianceTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, HitDistanceTexture);
}
