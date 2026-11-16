#include "DDGIRadianceCascadeRHIViewExtension.h"

#include "DDGIMasterSubsystem.h"
#include "DDGIRadianceCascadeResources.h"

#include "GlobalShader.h"
#include "ShaderParameterUtils.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "SceneInterface.h"
#include "SceneView.h"
#include "HAL/IConsoleManager.h"
#include "PostProcess/PostProcessMaterial.h"
#include "ScreenPass.h"

#include "RenderGraphUtils.h"
#include "RHIStaticStates.h"

static TAutoConsoleVariable<int32> CVarDDGIUsePrecomputedProbeNeighbors(
    TEXT("r.DDGI.UsePrecomputedProbeNeighbors"),
    1,
    TEXT("Use precomputed probe neighbors in DDGI spatial gather (0=spatial hash, 1=precomputed list; default 1)."),
    ECVF_RenderThreadSafe);

static const int32 GDDGISurfaceProbeMaxTraceBudget = 512;

static TAutoConsoleVariable<int32> CVarDDGISurfaceProbeTraceBudget(
    TEXT("r.DDGI.SurfaceProbeTraceBudget"),
    64,
    TEXT("Maximum retained surface probes scheduled for tracing per frame (0 disables surface-probe tracing schedule)."),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarDDGIDebugApply(
    TEXT("r.DDGI.DebugApply"),
    0,
    TEXT("DDGI apply debug view (0=off, 1=probe coverage, 2=raw irradiance, 3=solid magenta, 4-7=spatial-level irradiance)."),
    ECVF_RenderThreadSafe);

static void DDGITransitionTexture(
	FRHICommandList& RHICmdList,
	FRHITexture* Texture,
	ERHIAccess AccessBefore,
	ERHIAccess AccessAfter)
{
	if (!Texture) return;
	FRHITransitionInfo TransitionInfo(Texture, AccessBefore, AccessAfter);
	RHICmdList.Transition(MakeArrayView(&TransitionInfo, 1));
}

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
		DirectionsUAV.Bind(Initializer.ParameterMap, TEXT("DirectionsUAV"));
		RadianceUAV.Bind(Initializer.ParameterMap, TEXT("RadianceUAV"));
		HitDistanceUAV.Bind(Initializer.ParameterMap, TEXT("HitDistanceUAV"));
		RadianceScratchUAV.Bind(Initializer.ParameterMap, TEXT("RadianceScratchUAV"));
		RadianceIntegrationScratchUAV.Bind(Initializer.ParameterMap, TEXT("RadianceIntegrationScratchUAV"));
		HitTriangleIndexUAV.Bind(Initializer.ParameterMap, TEXT("HitTriangleIndexUAV"));
		NewPhysicalSlots.Bind(Initializer.ParameterMap, TEXT("NewPhysicalSlots"));
		bInitializeDirections.Bind(Initializer.ParameterMap, TEXT("bInitializeDirections"));
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	template<typename TShaderRHIParamRef>
	void SetParameters(
		FRHICommandList& RHICmdList,
		const TShaderRHIParamRef ShaderRHI,
		FUnorderedAccessViewRHIRef DirectionsView,
		FUnorderedAccessViewRHIRef RadianceView,
		FUnorderedAccessViewRHIRef HitDistanceView,
		FUnorderedAccessViewRHIRef RadianceScratchView,
		FUnorderedAccessViewRHIRef RadianceIntegrationScratchView,
		FUnorderedAccessViewRHIRef HitTriangleIndexView,
		FShaderResourceViewRHIRef InNewPhysicalSlotsSRV,
		uint32 bInInitializeDirections,
		uint32 InTotalRaysPerProbe,
		uint32 InCascadeCount,
		uint32 InBaseRayCount,
		uint32 InRaySplitFactor,
		uint32 InProbeCount,
		uint32 InTotalTexels,
		uint32 InDispatchGroupsX)
	{
		const uint32 InTextureWidth = 4096;

		SetShaderValue(RHICmdList, ShaderRHI, TotalRaysPerProbe, InTotalRaysPerProbe);
		SetShaderValue(RHICmdList, ShaderRHI, CascadeCount, InCascadeCount);
		SetShaderValue(RHICmdList, ShaderRHI, BaseRayCount, InBaseRayCount);
		SetShaderValue(RHICmdList, ShaderRHI, RaySplitFactor, InRaySplitFactor);
		SetShaderValue(RHICmdList, ShaderRHI, TextureWidth, InTextureWidth);
		SetShaderValue(RHICmdList, ShaderRHI, TotalTexels, InTotalTexels);
		SetShaderValue(RHICmdList, ShaderRHI, DispatchGroupsX, InDispatchGroupsX);
		SetShaderValue(RHICmdList, ShaderRHI, ProbeCount, InProbeCount);
		SetShaderValue(RHICmdList, ShaderRHI, bInitializeDirections, bInInitializeDirections);

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
		if (RadianceScratchUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, RadianceScratchUAV.GetBaseIndex(), RadianceScratchView);
		if (RadianceIntegrationScratchUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, RadianceIntegrationScratchUAV.GetBaseIndex(), RadianceIntegrationScratchView);
		if (HitTriangleIndexUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, HitTriangleIndexUAV.GetBaseIndex(), HitTriangleIndexView);
		if (NewPhysicalSlots.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, NewPhysicalSlots.GetBaseIndex(), InNewPhysicalSlotsSRV);
	}

	template<typename TShaderRHIParamRef>
	void UnbindResources(FRHICommandList& RHICmdList, const TShaderRHIParamRef ShaderRHI)
	{
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
		if (RadianceScratchUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, RadianceScratchUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
		if (RadianceIntegrationScratchUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, RadianceIntegrationScratchUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
		if (HitTriangleIndexUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, HitTriangleIndexUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
		if (NewPhysicalSlots.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, NewPhysicalSlots.GetBaseIndex(), FShaderResourceViewRHIRef());
	}

private:
	LAYOUT_FIELD(FShaderParameter, TotalRaysPerProbe);
	LAYOUT_FIELD(FShaderParameter, CascadeCount);
	LAYOUT_FIELD(FShaderParameter, BaseRayCount);
	LAYOUT_FIELD(FShaderParameter, RaySplitFactor);
	LAYOUT_FIELD(FShaderParameter, TextureWidth);
	LAYOUT_FIELD(FShaderParameter, TotalTexels);
	LAYOUT_FIELD(FShaderParameter, DispatchGroupsX);
	LAYOUT_FIELD(FShaderParameter, ProbeCount);
	LAYOUT_FIELD(FShaderResourceParameter, DirectionsUAV);
	LAYOUT_FIELD(FShaderResourceParameter, RadianceUAV);
	LAYOUT_FIELD(FShaderResourceParameter, HitDistanceUAV);
	LAYOUT_FIELD(FShaderResourceParameter, RadianceScratchUAV);
	LAYOUT_FIELD(FShaderResourceParameter, RadianceIntegrationScratchUAV);
	LAYOUT_FIELD(FShaderResourceParameter, HitTriangleIndexUAV);
	LAYOUT_FIELD(FShaderResourceParameter, NewPhysicalSlots);
	LAYOUT_FIELD(FShaderParameter, bInitializeDirections);
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
		CascadeCount.Bind(Initializer.ParameterMap, TEXT("CascadeCount"));
		BaseSegmentLength.Bind(Initializer.ParameterMap, TEXT("BaseSegmentLength"));
		CascadeScale.Bind(Initializer.ParameterMap, TEXT("CascadeScale"));
		MaxTraceDistance.Bind(Initializer.ParameterMap, TEXT("MaxTraceDistance"));
		StaticBackgroundColor.Bind(Initializer.ParameterMap, TEXT("StaticBackgroundColor"));
		StaticBackgroundIntensity.Bind(Initializer.ParameterMap, TEXT("StaticBackgroundIntensity"));
		GIIntensity.Bind(Initializer.ParameterMap, TEXT("GIIntensity"));
		ProbeGridOrigin.Bind(Initializer.ParameterMap, TEXT("ProbeGridOrigin"));
		ActiveWindowCenter.Bind(Initializer.ParameterMap, TEXT("ActiveWindowCenter"));
		BackgroundTransitionStart.Bind(Initializer.ParameterMap, TEXT("BackgroundTransitionStart"));
		BackgroundTransitionEnd.Bind(Initializer.ParameterMap, TEXT("BackgroundTransitionEnd"));
		bEnableStaticBackground.Bind(Initializer.ParameterMap, TEXT("bEnableStaticBackground"));

		DirectionalLightDirection.Bind(Initializer.ParameterMap, TEXT("DirectionalLightDirection"));
		DirectionalLightColor.Bind(Initializer.ParameterMap, TEXT("DirectionalLightColor"));
		DirectionalLightIntensity.Bind(Initializer.ParameterMap, TEXT("DirectionalLightIntensity"));
		bHasDirectionalLight.Bind(Initializer.ParameterMap, TEXT("bHasDirectionalLight"));
		DirectionsTexture.Bind(Initializer.ParameterMap, TEXT("DirectionsTexture"));
		ProbePositionsTexture.Bind(Initializer.ParameterMap, TEXT("ProbePositionsTexture"));
		LogicalToPhysicalSlots.Bind(Initializer.ParameterMap, TEXT("LogicalToPhysicalSlots"));
		BVHNodes.Bind(Initializer.ParameterMap, TEXT("BVHNodes"));
		BVHTriangles.Bind(Initializer.ParameterMap, TEXT("BVHTriangles"));
		RadianceUAV.Bind(Initializer.ParameterMap, TEXT("RadianceUAV"));
		HitDistanceUAV.Bind(Initializer.ParameterMap, TEXT("HitDistanceUAV"));
		HitTriangleIndexUAV.Bind(Initializer.ParameterMap, TEXT("HitTriangleIndexUAV"));
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	template<typename TShaderRHIParamRef>
	void SetParameters(
		FRHICommandList& RHICmdList,
		const TShaderRHIParamRef ShaderRHI,
		FShaderResourceViewRHIRef DirectionsSRV,
		FShaderResourceViewRHIRef ProbePositionsSRV,
		FShaderResourceViewRHIRef LogicalToPhysicalSRV,
		FShaderResourceViewRHIRef NodesSRV,
		FShaderResourceViewRHIRef TrianglesSRV,
		FUnorderedAccessViewRHIRef RadianceView,
		FUnorderedAccessViewRHIRef HitDistanceView,
		FUnorderedAccessViewRHIRef HitTriangleIndexView,
		uint32 InTotalTexels,
		uint32 InDispatchGroupsX,
		uint32 InProbeCount,
		uint32 InTotalRaysPerProbe,
		uint32 InNodeCount,
		uint32 InTriangleCount,
		uint32 InCascadeCount,
		float InBaseSegmentLength,
		float InCascadeScale,
		float InMaxTraceDistance,
		const FVector4& InStaticBackgroundColor,
		float InStaticBackgroundIntensity,
		float InGIIntensity,
		const FVector4& InProbeGridOrigin,
		const FVector4& InActiveWindowCenter,
		float InBackgroundTransitionStart,
		float InBackgroundTransitionEnd,
		uint32 bInEnableStaticBackground,
		const FVector4& InDirectionalLightDirection,
		const FVector4& InDirectionalLightColor,
		float InDirectionalLightIntensity,
		uint32 bInHasDirectionalLight)
	{
		const uint32 InTextureWidth = 4096;
		SetShaderValue(RHICmdList, ShaderRHI, TextureWidth, InTextureWidth);
		SetShaderValue(RHICmdList, ShaderRHI, TotalTexels, InTotalTexels);
		SetShaderValue(RHICmdList, ShaderRHI, DispatchGroupsX, InDispatchGroupsX);
		SetShaderValue(RHICmdList, ShaderRHI, ProbeCount, InProbeCount);
		SetShaderValue(RHICmdList, ShaderRHI, TotalRaysPerProbe, InTotalRaysPerProbe);
		SetShaderValue(RHICmdList, ShaderRHI, NodeCount, InNodeCount);
		SetShaderValue(RHICmdList, ShaderRHI, TriangleCount, InTriangleCount);
		SetShaderValue(RHICmdList, ShaderRHI, CascadeCount, InCascadeCount);
		SetShaderValue(RHICmdList, ShaderRHI, BaseSegmentLength, InBaseSegmentLength);
		SetShaderValue(RHICmdList, ShaderRHI, CascadeScale, InCascadeScale);
		SetShaderValue(RHICmdList, ShaderRHI, MaxTraceDistance, InMaxTraceDistance);
		SetShaderValue(RHICmdList, ShaderRHI, StaticBackgroundColor, InStaticBackgroundColor);
		SetShaderValue(RHICmdList, ShaderRHI, StaticBackgroundIntensity, InStaticBackgroundIntensity);
		SetShaderValue(RHICmdList, ShaderRHI, GIIntensity, InGIIntensity);
		SetShaderValue(RHICmdList, ShaderRHI, ProbeGridOrigin, InProbeGridOrigin);
		SetShaderValue(RHICmdList, ShaderRHI, ActiveWindowCenter, InActiveWindowCenter);
		SetShaderValue(RHICmdList, ShaderRHI, BackgroundTransitionStart, InBackgroundTransitionStart);
		SetShaderValue(RHICmdList, ShaderRHI, BackgroundTransitionEnd, InBackgroundTransitionEnd);
		SetShaderValue(RHICmdList, ShaderRHI, bEnableStaticBackground, bInEnableStaticBackground);

		SetShaderValue(RHICmdList, ShaderRHI, DirectionalLightDirection, InDirectionalLightDirection);
		SetShaderValue(RHICmdList, ShaderRHI, DirectionalLightColor, InDirectionalLightColor);
		SetShaderValue(RHICmdList, ShaderRHI, DirectionalLightIntensity, InDirectionalLightIntensity);
		SetShaderValue(RHICmdList, ShaderRHI, bHasDirectionalLight, bInHasDirectionalLight);

		if (DirectionsTexture.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, DirectionsTexture.GetBaseIndex(), DirectionsSRV);
		if (ProbePositionsTexture.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, ProbePositionsTexture.GetBaseIndex(), ProbePositionsSRV);
		if (LogicalToPhysicalSlots.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, LogicalToPhysicalSlots.GetBaseIndex(), LogicalToPhysicalSRV);
		if (BVHNodes.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, BVHNodes.GetBaseIndex(), NodesSRV);
		if (BVHTriangles.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, BVHTriangles.GetBaseIndex(), TrianglesSRV);
		if (RadianceUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, RadianceUAV.GetBaseIndex(), RadianceView);
		if (HitDistanceUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, HitDistanceUAV.GetBaseIndex(), HitDistanceView);
		if (HitTriangleIndexUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, HitTriangleIndexUAV.GetBaseIndex(), HitTriangleIndexView);
	}

	template<typename TShaderRHIParamRef>
	void UnbindResources(FRHICommandList& RHICmdList, const TShaderRHIParamRef ShaderRHI)
	{
		if (DirectionsTexture.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, DirectionsTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
		if (ProbePositionsTexture.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, ProbePositionsTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
		if (LogicalToPhysicalSlots.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, LogicalToPhysicalSlots.GetBaseIndex(), FShaderResourceViewRHIRef());
		if (BVHNodes.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, BVHNodes.GetBaseIndex(), FShaderResourceViewRHIRef());
		if (BVHTriangles.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, BVHTriangles.GetBaseIndex(), FShaderResourceViewRHIRef());
		if (RadianceUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, RadianceUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
		if (HitDistanceUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, HitDistanceUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
		if (HitTriangleIndexUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, HitTriangleIndexUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
	}

private:
	LAYOUT_FIELD(FShaderParameter, TextureWidth);
	LAYOUT_FIELD(FShaderParameter, TotalTexels);
	LAYOUT_FIELD(FShaderParameter, DispatchGroupsX);
	LAYOUT_FIELD(FShaderParameter, ProbeCount);
	LAYOUT_FIELD(FShaderParameter, TotalRaysPerProbe);
	LAYOUT_FIELD(FShaderParameter, NodeCount);
	LAYOUT_FIELD(FShaderParameter, TriangleCount);
	LAYOUT_FIELD(FShaderParameter, CascadeCount);
	LAYOUT_FIELD(FShaderParameter, BaseSegmentLength);
	LAYOUT_FIELD(FShaderParameter, CascadeScale);
	LAYOUT_FIELD(FShaderParameter, MaxTraceDistance);
	LAYOUT_FIELD(FShaderParameter, StaticBackgroundColor);
	LAYOUT_FIELD(FShaderParameter, StaticBackgroundIntensity);
	LAYOUT_FIELD(FShaderParameter, GIIntensity);
	LAYOUT_FIELD(FShaderParameter, ProbeGridOrigin);
	LAYOUT_FIELD(FShaderParameter, ActiveWindowCenter);
	LAYOUT_FIELD(FShaderParameter, BackgroundTransitionStart);
	LAYOUT_FIELD(FShaderParameter, BackgroundTransitionEnd);
	LAYOUT_FIELD(FShaderParameter, bEnableStaticBackground);

	LAYOUT_FIELD(FShaderParameter, DirectionalLightDirection);
	LAYOUT_FIELD(FShaderParameter, DirectionalLightColor);
	LAYOUT_FIELD(FShaderParameter, DirectionalLightIntensity);
	LAYOUT_FIELD(FShaderParameter, bHasDirectionalLight);
	LAYOUT_FIELD(FShaderResourceParameter, DirectionsTexture);
	LAYOUT_FIELD(FShaderResourceParameter, ProbePositionsTexture);
	LAYOUT_FIELD(FShaderResourceParameter, LogicalToPhysicalSlots);
	LAYOUT_FIELD(FShaderResourceParameter, BVHNodes);
	LAYOUT_FIELD(FShaderResourceParameter, BVHTriangles);
	LAYOUT_FIELD(FShaderResourceParameter, RadianceUAV);
	LAYOUT_FIELD(FShaderResourceParameter, HitDistanceUAV);
	LAYOUT_FIELD(FShaderResourceParameter, HitTriangleIndexUAV);
};

IMPLEMENT_SHADER_TYPE(
	,
	FDDGIRadianceCascadeTraceRHI,
	TEXT("/Plugin/DDGIRadianceCascades/Private/RadianceCascades/RadianceCascadeTraceCS.usf"),
	TEXT("RadianceCascadeTraceCS"),
	SF_Compute);


class FDDGIRadianceCascadeSpatialGatherRHI : public FGlobalShader
{
 DECLARE_SHADER_TYPE(FDDGIRadianceCascadeSpatialGatherRHI, Global);
public:
 FDDGIRadianceCascadeSpatialGatherRHI() {}
 explicit FDDGIRadianceCascadeSpatialGatherRHI(const ShaderMetaType::CompiledShaderInitializerType& I) : FGlobalShader(I)
 {
  TextureWidth.Bind(I.ParameterMap,TEXT("TextureWidth")); TotalTexels.Bind(I.ParameterMap,TEXT("TotalTexels"));
  DispatchGroupsX.Bind(I.ParameterMap,TEXT("DispatchGroupsX")); ProbeCount.Bind(I.ParameterMap,TEXT("ProbeCount"));
  TotalRaysPerProbe.Bind(I.ParameterMap,TEXT("TotalRaysPerProbe")); HashCapacity.Bind(I.ParameterMap,TEXT("HashCapacity"));
  TriangleCount.Bind(I.ParameterMap,TEXT("TriangleCount"));
  ProbeSpacing.Bind(I.ParameterMap,TEXT("ProbeSpacing")); ProbeGridOrigin.Bind(I.ParameterMap,TEXT("ProbeGridOrigin"));
  RadianceTexture.Bind(I.ParameterMap,TEXT("RadianceTexture")); HitDistanceTexture.Bind(I.ParameterMap,TEXT("HitDistanceTexture")); HitTriangleIndexTexture.Bind(I.ParameterMap,TEXT("HitTriangleIndexTexture"));
  DirectionsTexture.Bind(I.ParameterMap,TEXT("DirectionsTexture"));
  ProbePositionsTexture.Bind(I.ParameterMap,TEXT("ProbePositionsTexture")); BVHTriangles.Bind(I.ParameterMap,TEXT("BVHTriangles"));
  ProbeSpatialHash.Bind(I.ParameterMap,TEXT("ProbeSpatialHash"));
  ProbeNeighborIndices.Bind(I.ParameterMap,TEXT("ProbeNeighborIndices"));
  UsePrecomputedProbeNeighbors.Bind(I.ParameterMap,TEXT("UsePrecomputedProbeNeighbors"));
  OutputRadianceUAV.Bind(I.ParameterMap,TEXT("OutputRadianceUAV"));
  LogicalToPhysicalSlots.Bind(I.ParameterMap,TEXT("LogicalToPhysicalSlots"));
 }
 static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P){return IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM5);}
 template<typename TShaderRHIParamRef>
 void SetParameters(FRHICommandList& C,const TShaderRHIParamRef Sh,FShaderResourceViewRHIRef R,FShaderResourceViewRHIRef H,FShaderResourceViewRHIRef HitIndices,
  FShaderResourceViewRHIRef D,FShaderResourceViewRHIRef P,FShaderResourceViewRHIRef LogicalToPhysicalSRV,FShaderResourceViewRHIRef Triangles,FShaderResourceViewRHIRef S,
  FShaderResourceViewRHIRef NeighborList,FUnorderedAccessViewRHIRef O,
  uint32 T,uint32 G,uint32 PC,uint32 RPP,uint32 HC,uint32 TC,float PS,const FVector4& Origin,uint32 bUseNeighborList)
 {
  uint32 W=4096u;
  SetShaderValue(C,Sh,TextureWidth,W); SetShaderValue(C,Sh,TotalTexels,T); SetShaderValue(C,Sh,DispatchGroupsX,G);
  SetShaderValue(C,Sh,ProbeCount,PC); SetShaderValue(C,Sh,TotalRaysPerProbe,RPP); SetShaderValue(C,Sh,HashCapacity,HC);
  SetShaderValue(C,Sh,TriangleCount,TC);
  SetShaderValue(C,Sh,ProbeSpacing,PS); SetShaderValue(C,Sh,ProbeGridOrigin,Origin);
  SetShaderValue(C,Sh,UsePrecomputedProbeNeighbors,bUseNeighborList);
  if(RadianceTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,RadianceTexture.GetBaseIndex(),R);
  if(HitDistanceTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,HitDistanceTexture.GetBaseIndex(),H);
  if(HitTriangleIndexTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,HitTriangleIndexTexture.GetBaseIndex(),HitIndices);
  if(DirectionsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,DirectionsTexture.GetBaseIndex(),D);
  
  if(ProbePositionsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,ProbePositionsTexture.GetBaseIndex(),P);
  if(LogicalToPhysicalSlots.IsBound()) C.SetShaderResourceViewParameter(Sh,LogicalToPhysicalSlots.GetBaseIndex(),LogicalToPhysicalSRV);
  if(BVHTriangles.IsBound()) C.SetShaderResourceViewParameter(Sh,BVHTriangles.GetBaseIndex(),Triangles);
  if(ProbeSpatialHash.IsBound()) C.SetShaderResourceViewParameter(Sh,ProbeSpatialHash.GetBaseIndex(),S);
  if(ProbeNeighborIndices.IsBound()) C.SetShaderResourceViewParameter(Sh,ProbeNeighborIndices.GetBaseIndex(),NeighborList);
  if(OutputRadianceUAV.IsBound()) C.SetUAVParameter(Sh,OutputRadianceUAV.GetBaseIndex(),O);
 }
 template<typename TShaderRHIParamRef>
 void UnbindResources(FRHICommandList& C,const TShaderRHIParamRef Sh)
 {
  if(RadianceTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,RadianceTexture.GetBaseIndex(),FShaderResourceViewRHIRef());
  if(HitDistanceTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,HitDistanceTexture.GetBaseIndex(),FShaderResourceViewRHIRef());
  if(HitTriangleIndexTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,HitTriangleIndexTexture.GetBaseIndex(),FShaderResourceViewRHIRef());
  if(DirectionsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,DirectionsTexture.GetBaseIndex(),FShaderResourceViewRHIRef());
  
  if(ProbePositionsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,ProbePositionsTexture.GetBaseIndex(),FShaderResourceViewRHIRef());
  if(LogicalToPhysicalSlots.IsBound()) C.SetShaderResourceViewParameter(Sh,LogicalToPhysicalSlots.GetBaseIndex(),FShaderResourceViewRHIRef());
  if(BVHTriangles.IsBound()) C.SetShaderResourceViewParameter(Sh,BVHTriangles.GetBaseIndex(),FShaderResourceViewRHIRef());
  if(ProbeSpatialHash.IsBound()) C.SetShaderResourceViewParameter(Sh,ProbeSpatialHash.GetBaseIndex(),FShaderResourceViewRHIRef());
  if(ProbeNeighborIndices.IsBound()) C.SetShaderResourceViewParameter(Sh,ProbeNeighborIndices.GetBaseIndex(),FShaderResourceViewRHIRef());
  if(OutputRadianceUAV.IsBound()) C.SetUAVParameter(Sh,OutputRadianceUAV.GetBaseIndex(),FUnorderedAccessViewRHIRef());
 }
private:
 LAYOUT_FIELD(FShaderParameter, TextureWidth);
 LAYOUT_FIELD(FShaderParameter, TotalTexels);
 LAYOUT_FIELD(FShaderParameter, DispatchGroupsX);
 LAYOUT_FIELD(FShaderParameter, ProbeCount);
 LAYOUT_FIELD(FShaderParameter, TotalRaysPerProbe);
 LAYOUT_FIELD(FShaderParameter, HashCapacity);
 LAYOUT_FIELD(FShaderParameter, TriangleCount);
 LAYOUT_FIELD(FShaderParameter, ProbeSpacing);
 LAYOUT_FIELD(FShaderParameter, ProbeGridOrigin);
 LAYOUT_FIELD(FShaderResourceParameter, RadianceTexture);
 LAYOUT_FIELD(FShaderResourceParameter, HitDistanceTexture);
 LAYOUT_FIELD(FShaderResourceParameter, HitTriangleIndexTexture);
 LAYOUT_FIELD(FShaderResourceParameter, DirectionsTexture);
 LAYOUT_FIELD(FShaderResourceParameter, ProbePositionsTexture);
 LAYOUT_FIELD(FShaderResourceParameter, LogicalToPhysicalSlots);
 LAYOUT_FIELD(FShaderResourceParameter, BVHTriangles);
 LAYOUT_FIELD(FShaderResourceParameter, ProbeSpatialHash);
 LAYOUT_FIELD(FShaderResourceParameter, ProbeNeighborIndices);
 LAYOUT_FIELD(FShaderResourceParameter, OutputRadianceUAV);
 LAYOUT_FIELD(FShaderParameter, UsePrecomputedProbeNeighbors);
};
IMPLEMENT_SHADER_TYPE(,FDDGIRadianceCascadeSpatialGatherRHI,
 TEXT("/Plugin/DDGIRadianceCascades/Private/RadianceCascades/RadianceCascadeSpatialGatherCS.usf"),
 TEXT("RadianceCascadeSpatialGatherCS"),SF_Compute);

class FDDGIRadianceCascadeIntegrateRHI : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FDDGIRadianceCascadeIntegrateRHI, Global);

public:
	FDDGIRadianceCascadeIntegrateRHI() {}

	explicit FDDGIRadianceCascadeIntegrateRHI(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
		TextureWidth.Bind(Initializer.ParameterMap, TEXT("TextureWidth"));
		TotalIntegrationTexels.Bind(Initializer.ParameterMap, TEXT("TotalIntegrationTexels"));
		DispatchGroupsX.Bind(Initializer.ParameterMap, TEXT("DispatchGroupsX"));
		TotalRaysPerProbe.Bind(Initializer.ParameterMap, TEXT("TotalRaysPerProbe"));
		CascadeStorageOffset.Bind(Initializer.ParameterMap, TEXT("CascadeStorageOffset"));
		CascadeRayCount.Bind(Initializer.ParameterMap, TEXT("CascadeRayCount"));
		NextCascadeStorageOffset.Bind(Initializer.ParameterMap, TEXT("NextCascadeStorageOffset"));
		EffectiveSplitFactor.Bind(Initializer.ParameterMap, TEXT("EffectiveSplitFactor"));
		RadianceUAV.Bind(Initializer.ParameterMap, TEXT("RadianceUAV"));
		GatheredRadianceTexture.Bind(Initializer.ParameterMap, TEXT("GatheredRadianceTexture"));
		ChildRadianceTexture.Bind(Initializer.ParameterMap, TEXT("ChildRadianceTexture"));
		HitDistanceTexture.Bind(Initializer.ParameterMap, TEXT("HitDistanceTexture"));
		LogicalToPhysicalSlots.Bind(Initializer.ParameterMap, TEXT("LogicalToPhysicalSlots"));
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	template<typename TShaderRHIParamRef>
	void SetParameters(
		FRHICommandList& RHICmdList,
		const TShaderRHIParamRef ShaderRHI,
		FUnorderedAccessViewRHIRef InRadianceUAV,
		FShaderResourceViewRHIRef GatheredRadianceSRV,
		FShaderResourceViewRHIRef ChildRadianceSRV,
		FShaderResourceViewRHIRef HitDistanceSRV,
		FShaderResourceViewRHIRef LogicalToPhysicalSRV,
		uint32 InTotalIntegrationTexels,
		uint32 InDispatchGroupsX,
		uint32 InTotalRaysPerProbe,
		uint32 InCascadeStorageOffset,
		uint32 InCascadeRayCount,
		uint32 InNextCascadeStorageOffset,
		uint32 InEffectiveSplitFactor)
	{
		const uint32 InTextureWidth = 4096u;
		SetShaderValue(RHICmdList, ShaderRHI, TextureWidth, InTextureWidth);
		SetShaderValue(RHICmdList, ShaderRHI, TotalIntegrationTexels, InTotalIntegrationTexels);
		SetShaderValue(RHICmdList, ShaderRHI, DispatchGroupsX, InDispatchGroupsX);
		SetShaderValue(RHICmdList, ShaderRHI, TotalRaysPerProbe, InTotalRaysPerProbe);
		SetShaderValue(RHICmdList, ShaderRHI, CascadeStorageOffset, InCascadeStorageOffset);
		SetShaderValue(RHICmdList, ShaderRHI, CascadeRayCount, InCascadeRayCount);
		SetShaderValue(RHICmdList, ShaderRHI, NextCascadeStorageOffset, InNextCascadeStorageOffset);
		SetShaderValue(RHICmdList, ShaderRHI, EffectiveSplitFactor, InEffectiveSplitFactor);
		if (RadianceUAV.IsBound())
		{
			RHICmdList.SetUAVParameter(ShaderRHI, RadianceUAV.GetBaseIndex(), InRadianceUAV);
		}
		if (GatheredRadianceTexture.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, GatheredRadianceTexture.GetBaseIndex(), GatheredRadianceSRV);
		}
		if (ChildRadianceTexture.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, ChildRadianceTexture.GetBaseIndex(), ChildRadianceSRV);
		}
		if (HitDistanceTexture.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, HitDistanceTexture.GetBaseIndex(), HitDistanceSRV);
		}
		if (LogicalToPhysicalSlots.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, LogicalToPhysicalSlots.GetBaseIndex(), LogicalToPhysicalSRV);
		}
	}

	template<typename TShaderRHIParamRef>
	void UnbindResources(FRHICommandList& RHICmdList, const TShaderRHIParamRef ShaderRHI)
	{
		if (RadianceUAV.IsBound())
		{
			RHICmdList.SetUAVParameter(ShaderRHI, RadianceUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
		}
		if (GatheredRadianceTexture.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, GatheredRadianceTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
		}
		if (ChildRadianceTexture.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, ChildRadianceTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
		}
		if (HitDistanceTexture.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, HitDistanceTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
		}
		if (LogicalToPhysicalSlots.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, LogicalToPhysicalSlots.GetBaseIndex(), FShaderResourceViewRHIRef());
		}
	}

private:
	LAYOUT_FIELD(FShaderParameter, TextureWidth);
	LAYOUT_FIELD(FShaderParameter, TotalIntegrationTexels);
	LAYOUT_FIELD(FShaderParameter, DispatchGroupsX);
	LAYOUT_FIELD(FShaderParameter, TotalRaysPerProbe);
	LAYOUT_FIELD(FShaderParameter, CascadeStorageOffset);
	LAYOUT_FIELD(FShaderParameter, CascadeRayCount);
	LAYOUT_FIELD(FShaderParameter, NextCascadeStorageOffset);
	LAYOUT_FIELD(FShaderParameter, EffectiveSplitFactor);
	LAYOUT_FIELD(FShaderResourceParameter, RadianceUAV);
	LAYOUT_FIELD(FShaderResourceParameter, GatheredRadianceTexture);
	LAYOUT_FIELD(FShaderResourceParameter, ChildRadianceTexture);
	LAYOUT_FIELD(FShaderResourceParameter, HitDistanceTexture);
	LAYOUT_FIELD(FShaderResourceParameter, LogicalToPhysicalSlots);
};

IMPLEMENT_SHADER_TYPE(
	,
	FDDGIRadianceCascadeIntegrateRHI,
	TEXT("/Plugin/DDGIRadianceCascades/Private/RadianceCascades/RadianceCascadeIntegrateCS.usf"),
	TEXT("RadianceCascadeIntegrateCS"),
	SF_Compute);



class FDDGIApplyRadianceCascadesLightingCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDDGIApplyRadianceCascadesLightingCS);
	SHADER_USE_PARAMETER_STRUCT(FDDGIApplyRadianceCascadesLightingCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NormalTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, BaseColorTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, MetallicTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DirectionsTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, RadianceTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ProbePositionsTexture)
		SHADER_PARAMETER_SRV(StructuredBuffer<int4>, ProbeSpatialHash)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, LogicalToPhysicalSlots)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, SceneColorOutputUAV)
		SHADER_PARAMETER_SAMPLER(SamplerState, PointClampSampler)
		SHADER_PARAMETER(FVector4, ScaledViewSizeAndInvSize)
		SHADER_PARAMETER(FVector2D, ViewRectMin)
		SHADER_PARAMETER(uint32, ProbeCount)
		SHADER_PARAMETER(uint32, TotalRaysPerProbe)
		SHADER_PARAMETER(uint32, BaseRayCount)
		SHADER_PARAMETER(uint32, HashCapacity)
		SHADER_PARAMETER(float, ProbeSpacing)
		SHADER_PARAMETER(float, ProbeGridDistance)
		SHADER_PARAMETER(FVector4, ProbeGridOrigin)
		SHADER_PARAMETER(FVector4, ActiveWindowCenter)
		SHADER_PARAMETER(FVector4, BVHWorldOrigin)
		SHADER_PARAMETER(uint32, ShouldUsePreExposure)
		SHADER_PARAMETER(uint32, DebugMode)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};


class FDDGISurfaceProbeCaptureCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FDDGISurfaceProbeCaptureCS);
    SHADER_USE_PARAMETER_STRUCT(FDDGISurfaceProbeCaptureCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NormalTexture)
        SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DepthTexture)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfaceProbeCandidatePositions)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, SurfaceProbeCandidateNormals)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SurfaceProbeCounters)
        SHADER_PARAMETER_SAMPLER(SamplerState, PointClampSampler)
        SHADER_PARAMETER(FVector4, ScaledViewSizeAndInvSize)
        SHADER_PARAMETER(FVector2D, ViewRectMin)
        SHADER_PARAMETER(uint32, CaptureStride)
        SHADER_PARAMETER(uint32, CaptureGridWidth)
        SHADER_PARAMETER(uint32, CaptureGridHeight)
        SHADER_PARAMETER(uint32, CandidateCount)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

class FDDGISurfaceProbeBuildActiveListCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FDDGISurfaceProbeBuildActiveListCS);
    SHADER_USE_PARAMETER_STRUCT(FDDGISurfaceProbeBuildActiveListCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SurfaceProbeLastSeenFrames)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SurfaceProbeActiveIndices)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SurfaceProbeCounters)
        SHADER_PARAMETER(uint32, ProbeCapacity)
        SHADER_PARAMETER(uint32, FrameNumber)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

IMPLEMENT_GLOBAL_SHADER(
    FDDGISurfaceProbeBuildActiveListCS,
    "/Plugin/DDGIRadianceCascades/Private/RadianceCascades/SurfaceProbeBuildActiveList.usf",
    "SurfaceProbeBuildActiveListCS",
    SF_Compute);

class FDDGISurfaceProbeScheduleTraceCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FDDGISurfaceProbeScheduleTraceCS);
    SHADER_USE_PARAMETER_STRUCT(FDDGISurfaceProbeScheduleTraceCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SurfaceProbeActiveIndices)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SurfaceProbeTraceIndices)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SurfaceProbeCounters)
        SHADER_PARAMETER(uint32, ProbeCapacity)
        SHADER_PARAMETER(uint32, TraceProbeBudget)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

IMPLEMENT_GLOBAL_SHADER(
    FDDGISurfaceProbeScheduleTraceCS,
    "/Plugin/DDGIRadianceCascades/Private/RadianceCascades/SurfaceProbeBuildActiveList.usf",
    "SurfaceProbeScheduleTraceCS",
    SF_Compute);



static void DDGIScheduleSurfaceProbeCapture(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    const FPostProcessMaterialInputs& Inputs,
    const FScreenPassTexture& SceneColor,
    const FSceneInterface* Scene)
{
    if (!Scene || !View.Family || !Inputs.SceneTextures.SceneTextures || !SceneColor.IsValid())
    {
        return;
    }

    const FIntPoint ViewSize = SceneColor.ViewRect.Size();
    if (ViewSize.X <= 0 || ViewSize.Y <= 0)
    {
        return;
    }

    FDDGISurfaceProbeAllocatorRDGResources Resources;
    if (!DDGIRegisterSurfaceProbeAllocatorBuffers_RenderThread(GraphBuilder, Scene, Resources) ||
        !Resources.CandidatePositions || !Resources.CandidateNormals || !Resources.Counters)
    {
        return;
    }

    uint32 CaptureStride = 16u;
    uint32 CaptureGridWidth = 0u;
    uint32 CaptureGridHeight = 0u;
    uint32 CandidateCount = 0u;
    do
    {
        CaptureGridWidth = FMath::DivideAndRoundUp(static_cast<uint32>(ViewSize.X), CaptureStride);
        CaptureGridHeight = FMath::DivideAndRoundUp(static_cast<uint32>(ViewSize.Y), CaptureStride);
        CandidateCount = CaptureGridWidth * CaptureGridHeight;
        if (CandidateCount <= Resources.ProbeCapacity)
        {
            break;
        }
        ++CaptureStride;
    }
    while (CaptureStride <= static_cast<uint32>(FMath::Max(ViewSize.X, ViewSize.Y)));

    if (CandidateCount == 0u || CandidateCount > Resources.ProbeCapacity ||
        !DDGIBeginSurfaceProbeCapture_RenderThread(Scene, View.Family->FrameNumber))
    {
        return;
    }

    FDDGISurfaceProbeCaptureCS::FParameters* PassParameters =
        GraphBuilder.AllocParameters<FDDGISurfaceProbeCaptureCS::FParameters>();
    PassParameters->View = View.ViewUniformBuffer;
    PassParameters->NormalTexture = (*Inputs.SceneTextures.SceneTextures)->GBufferATexture;
    PassParameters->DepthTexture = (*Inputs.SceneTextures.SceneTextures)->SceneDepthTexture;
    PassParameters->SurfaceProbeCandidatePositions = GraphBuilder.CreateUAV(Resources.CandidatePositions);
    PassParameters->SurfaceProbeCandidateNormals = GraphBuilder.CreateUAV(Resources.CandidateNormals);
    PassParameters->SurfaceProbeCounters = GraphBuilder.CreateUAV(Resources.Counters);
    PassParameters->PointClampSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
    PassParameters->ScaledViewSizeAndInvSize = FVector4(
        static_cast<float>(ViewSize.X), static_cast<float>(ViewSize.Y),
        1.0f / static_cast<float>(ViewSize.X), 1.0f / static_cast<float>(ViewSize.Y));
    PassParameters->ViewRectMin = FVector2D(
        static_cast<float>(SceneColor.ViewRect.Min.X),
        static_cast<float>(SceneColor.ViewRect.Min.Y));
    PassParameters->CaptureStride = CaptureStride;
    PassParameters->CaptureGridWidth = CaptureGridWidth;
    PassParameters->CaptureGridHeight = CaptureGridHeight;
    PassParameters->CandidateCount = CandidateCount;

    TShaderMapRef<FDDGISurfaceProbeCaptureCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("DDGI Capture Surface Probe Candidates"),
        ComputeShader,
        PassParameters,
        FIntVector(
            FMath::DivideAndRoundUp(static_cast<int32>(CaptureGridWidth), 8),
            FMath::DivideAndRoundUp(static_cast<int32>(CaptureGridHeight), 8),
            1));
    FDDGISurfaceProbeDeduplicateCS::FParameters* DeduplicateParameters =
        GraphBuilder.AllocParameters<FDDGISurfaceProbeDeduplicateCS::FParameters>();
    DeduplicateParameters->SurfaceProbeCandidatePositions = GraphBuilder.CreateSRV(Resources.CandidatePositions);
    DeduplicateParameters->SurfaceProbeCandidateNormals = GraphBuilder.CreateSRV(Resources.CandidateNormals);
    DeduplicateParameters->SurfaceProbePositions = GraphBuilder.CreateUAV(Resources.Positions);
    DeduplicateParameters->SurfaceProbeNormals = GraphBuilder.CreateUAV(Resources.Normals);
    DeduplicateParameters->SurfaceProbeStableKeys = GraphBuilder.CreateUAV(Resources.StableKeys);
    DeduplicateParameters->SurfaceProbeLastSeenFrames = GraphBuilder.CreateUAV(Resources.LastSeenFrames);
    DeduplicateParameters->SurfaceProbeHash = GraphBuilder.CreateUAV(Resources.Hash);
    DeduplicateParameters->SurfaceProbeCounters = GraphBuilder.CreateUAV(Resources.Counters);
    DeduplicateParameters->ProbeCapacity = Resources.ProbeCapacity;
    DeduplicateParameters->HashCapacity = Resources.HashCapacity;
    DeduplicateParameters->FrameNumber = View.Family->FrameNumber;

    TShaderMapRef<FDDGISurfaceProbeDeduplicateCS> DeduplicateShader(GetGlobalShaderMap(View.GetFeatureLevel()));
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("DDGI Deduplicate Surface Probes"),
        DeduplicateShader,
        DeduplicateParameters,
        FIntVector(FMath::DivideAndRoundUp(static_cast<int32>(CandidateCount), 64), 1, 1));

    // Build a stable, ascending physical-slot list so the round-robin cursor
    // cannot starve probes when parallel append order changes between frames.
    FDDGISurfaceProbeBuildActiveListCS::FParameters* BuildActiveListParameters =
        GraphBuilder.AllocParameters<FDDGISurfaceProbeBuildActiveListCS::FParameters>();
    BuildActiveListParameters->SurfaceProbeLastSeenFrames = GraphBuilder.CreateSRV(Resources.LastSeenFrames);
    BuildActiveListParameters->SurfaceProbeActiveIndices = GraphBuilder.CreateUAV(Resources.ActiveIndices);
    BuildActiveListParameters->SurfaceProbeCounters = GraphBuilder.CreateUAV(Resources.Counters);
    BuildActiveListParameters->ProbeCapacity = Resources.ProbeCapacity;
    BuildActiveListParameters->FrameNumber = View.Family->FrameNumber;

    TShaderMapRef<FDDGISurfaceProbeBuildActiveListCS> BuildActiveListShader(GetGlobalShaderMap(View.GetFeatureLevel()));
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("DDGI Build Surface Probe Active List"),
        BuildActiveListShader,
        BuildActiveListParameters,
        FIntVector(1, 1, 1));
 
    // Schedule a bounded round-robin subset. The compact list contains physical
    // slots; this second list is the per-frame work queue consumed by future tracing.
    FDDGISurfaceProbeScheduleTraceCS::FParameters* ScheduleTraceParameters =
        GraphBuilder.AllocParameters<FDDGISurfaceProbeScheduleTraceCS::FParameters>();
    ScheduleTraceParameters->SurfaceProbeActiveIndices = GraphBuilder.CreateSRV(Resources.ActiveIndices);
    ScheduleTraceParameters->SurfaceProbeTraceIndices = GraphBuilder.CreateUAV(Resources.TraceIndices);
    ScheduleTraceParameters->SurfaceProbeCounters = GraphBuilder.CreateUAV(Resources.Counters);
    ScheduleTraceParameters->ProbeCapacity = Resources.ProbeCapacity;
    ScheduleTraceParameters->TraceProbeBudget = static_cast<uint32>(
        FMath::Clamp(CVarDDGISurfaceProbeTraceBudget.GetValueOnRenderThread(), 0, GDDGISurfaceProbeMaxTraceBudget));

    TShaderMapRef<FDDGISurfaceProbeScheduleTraceCS> ScheduleTraceShader(GetGlobalShaderMap(View.GetFeatureLevel()));
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("DDGI Schedule Surface Probe Tracing"),
        ScheduleTraceShader,
        ScheduleTraceParameters,
        FIntVector(1, 1, 1));
}

IMPLEMENT_GLOBAL_SHADER(
	FDDGIApplyRadianceCascadesLightingCS,
	"/Plugin/DDGIRadianceCascades/Private/ApplyRadianceCascadesLighting.usf",
	"MainCS",
	SF_Compute);

static FScreenPassTexture DDGIReturnSceneColorForPostProcessing(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs,
	const FScreenPassTexture& SceneColor)
{
	// The engine may provide a dedicated final output target. It must be
	// populated even when DDGI is disabled or its resources are unavailable.
	if (Inputs.OverrideOutput.IsValid() && SceneColor.IsValid())
	{
		AddDrawTexturePass(
			GraphBuilder,
			static_cast<const FViewInfo&>(View),
			SceneColor.Texture,
			Inputs.OverrideOutput.Texture,
			SceneColor.ViewRect.Min,
			Inputs.OverrideOutput.ViewRect.Min,
			SceneColor.ViewRect.Size());
		return FScreenPassTexture(
			Inputs.OverrideOutput.Texture,
			Inputs.OverrideOutput.ViewRect);
	}
	return SceneColor;
}

static FScreenPassTexture DDGIApplyRadianceCascadesAfterMotionBlur(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs)
{
	static bool bLoggedApplyCallback = false;
	if (!bLoggedApplyCallback)
	{
		UE_LOG(LogTemp, Warning, TEXT("DDGI DIAG: post-process apply callback executed."));
		bLoggedApplyCallback = true;
	}
	const FScreenPassTexture SceneColor = Inputs.GetInput(EPostProcessMaterialInput::SceneColor);
	if (!SceneColor.IsValid() || !View.Family || !View.Family->Scene)
	{
		static bool bLoggedInvalidView = false;
		if (!bLoggedInvalidView)
		{
			UE_LOG(LogTemp, Warning, TEXT("DDGI DIAG: apply callback has invalid SceneColor, ViewFamily, or Scene."));
			bLoggedInvalidView = true;
		}
		return DDGIReturnSceneColorForPostProcessing(GraphBuilder, View, Inputs, SceneColor);
	}

	const FSceneInterface* Scene = View.Family->Scene;
	const FDDGIMasterConfig* Config = DDGIGetMasterConfig_RenderThread(Scene);
	if (Config && Config->bEnableDynamicGI)
	{
		DDGIScheduleSurfaceProbeCapture(GraphBuilder, View, Inputs, SceneColor, Scene);
	}
	if (!Config || !Config->bEnableDynamicGI || !Config->SoftwareBVH.IsValid())
	{
		static bool bLoggedMissingConfig = false;
		if (!bLoggedMissingConfig)
		{
			UE_LOG(LogTemp, Warning, TEXT("DDGI DIAG: apply callback has no enabled master config or valid software BVH."));
			bLoggedMissingConfig = true;
		}
		return DDGIReturnSceneColorForPostProcessing(GraphBuilder, View, Inputs, SceneColor);
	}

	FRDGTextureRef DirectionsTexture = nullptr;
	FRDGTextureRef RadianceTexture = nullptr;
	FRDGTextureRef HitDistanceTexture = nullptr;
	FRDGTextureRef HitTriangleIndexTexture = nullptr;
	FRDGTextureRef ProbePositionsTexture = nullptr;
	uint32 ProbeCount = 0;
	uint32 TotalRaysPerProbe = 0;
	if (!DDGIGetRadianceCascadeLightingTextures_RenderThread(
		GraphBuilder, Scene, DirectionsTexture, RadianceTexture, HitDistanceTexture,
		HitTriangleIndexTexture, ProbePositionsTexture, ProbeCount, TotalRaysPerProbe))
	{
		static bool bLoggedMissingLightingTextures = false;
		if (!bLoggedMissingLightingTextures)
		{
			UE_LOG(LogTemp, Warning, TEXT("DDGI DIAG: apply callback cannot acquire completed radiance/probe textures."));
			bLoggedMissingLightingTextures = true;
		}
		return DDGIReturnSceneColorForPostProcessing(GraphBuilder, View, Inputs, SceneColor);
	}

	FShaderResourceViewRHIRef ProbeSpatialHashSRV;
	FShaderResourceViewRHIRef LogicalToPhysicalSRV, NewPhysicalSlotsSRV;
	uint32 NewPhysicalSlotCount = 0, PhysicalSlotCapacity = 0;
	uint32 HashCapacity = 0;
	float ProbeSpacing = 0.0f;
	if (!DDGIGetRadianceCascadePhysicalSlotResources_RenderThread(
		Scene, LogicalToPhysicalSRV, NewPhysicalSlotsSRV, NewPhysicalSlotCount, PhysicalSlotCapacity) ||
		!LogicalToPhysicalSRV.IsValid() || PhysicalSlotCapacity == 0u)
	{
		return DDGIReturnSceneColorForPostProcessing(GraphBuilder, View, Inputs, SceneColor);
	}

	if (!DDGIGetRadianceCascadeProbeSpatialHash_RenderThread(
		Scene, ProbeSpatialHashSRV, HashCapacity, ProbeSpacing) ||
		!ProbeSpatialHashSRV.IsValid() || HashCapacity == 0)
	{
		static bool bLoggedMissingHash = false;
		if (!bLoggedMissingHash)
		{
			UE_LOG(LogTemp, Warning, TEXT("DDGI DIAG: apply callback cannot acquire a valid probe spatial hash."));
			bLoggedMissingHash = true;
		}
		return DDGIReturnSceneColorForPostProcessing(GraphBuilder, View, Inputs, SceneColor);
	}

	if (!Inputs.SceneTextures.SceneTextures)
	{
		static bool bLoggedMissingSceneTextures = false;
		if (!bLoggedMissingSceneTextures)
		{
			UE_LOG(LogTemp, Warning, TEXT("DDGI DIAG: post-process inputs do not contain SceneTextures."));
			bLoggedMissingSceneTextures = true;
		}
		return DDGIReturnSceneColorForPostProcessing(GraphBuilder, View, Inputs, SceneColor);
	}

	const FIntPoint ViewSize = SceneColor.ViewRect.Size();
	if (ViewSize.X <= 0 || ViewSize.Y <= 0)
	{
		return DDGIReturnSceneColorForPostProcessing(GraphBuilder, View, Inputs, SceneColor);
	}

	FRDGTextureDesc OutputDesc = SceneColor.Texture->Desc;
	OutputDesc.Flags |= TexCreate_UAV | TexCreate_ShaderResource;
	FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(
		OutputDesc, TEXT("DDGI.RadianceCascades.SceneColor"));

	FDDGIApplyRadianceCascadesLightingCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FDDGIApplyRadianceCascadesLightingCS::FParameters>();
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->SceneColorTexture = SceneColor.Texture;
	PassParameters->NormalTexture = (*Inputs.SceneTextures.SceneTextures)->GBufferATexture;
	PassParameters->DepthTexture = (*Inputs.SceneTextures.SceneTextures)->SceneDepthTexture;
	PassParameters->BaseColorTexture = (*Inputs.SceneTextures.SceneTextures)->GBufferCTexture;
	PassParameters->MetallicTexture = (*Inputs.SceneTextures.SceneTextures)->GBufferBTexture;
	PassParameters->DirectionsTexture = DirectionsTexture;
	PassParameters->RadianceTexture = RadianceTexture;
	PassParameters->ProbePositionsTexture = ProbePositionsTexture;
	PassParameters->ProbeSpatialHash = ProbeSpatialHashSRV;
	PassParameters->LogicalToPhysicalSlots = LogicalToPhysicalSRV;
	PassParameters->SceneColorOutputUAV = GraphBuilder.CreateUAV(OutputTexture);
	PassParameters->PointClampSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->ScaledViewSizeAndInvSize = FVector4(
		static_cast<float>(ViewSize.X), static_cast<float>(ViewSize.Y),
		1.0f / static_cast<float>(ViewSize.X), 1.0f / static_cast<float>(ViewSize.Y));
	PassParameters->ViewRectMin = FVector2D(
		static_cast<float>(SceneColor.ViewRect.Min.X),
		static_cast<float>(SceneColor.ViewRect.Min.Y));
	PassParameters->ProbeCount = ProbeCount;
	PassParameters->TotalRaysPerProbe = TotalRaysPerProbe;
	PassParameters->BaseRayCount = static_cast<uint32>(FMath::Clamp(
		Config->RadianceCascades.BaseRayCount, 1, 64));
	PassParameters->HashCapacity = HashCapacity;
	PassParameters->ProbeSpacing = ProbeSpacing;
	PassParameters->ProbeGridDistance = Config->ProbeGridDistance;
	PassParameters->ProbeGridOrigin = FVector4(
		Config->ProbeGridOrigin.X, Config->ProbeGridOrigin.Y, Config->ProbeGridOrigin.Z, 0.0f);
	// Use the snapped active-window center, not the live camera origin. This keeps cascade blend weights stable while the camera moves inside the current probe window.
	const FVector ActiveWindowCenterLocal = Config->ActiveProbeWindowCenter - Config->SoftwareBVH->WorldOrigin;
	PassParameters->ActiveWindowCenter = FVector4(
		ActiveWindowCenterLocal.X, ActiveWindowCenterLocal.Y, ActiveWindowCenterLocal.Z, 0.0f);
	PassParameters->BVHWorldOrigin = FVector4(
		Config->SoftwareBVH->WorldOrigin.X,
		Config->SoftwareBVH->WorldOrigin.Y,
		Config->SoftwareBVH->WorldOrigin.Z, 0.0f);
	PassParameters->ShouldUsePreExposure = 1u;
	PassParameters->DebugMode = static_cast<uint32>(FMath::Clamp(CVarDDGIDebugApply.GetValueOnRenderThread(), 0, 7));

	TShaderMapRef<FDDGIApplyRadianceCascadesLightingCS> ComputeShader(
		GetGlobalShaderMap(View.GetFeatureLevel()));
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("DDGI Apply Radiance Cascades Lighting"),
		ComputeShader,
		PassParameters,
		FIntVector(
			FMath::DivideAndRoundUp(ViewSize.X, 8),
			FMath::DivideAndRoundUp(ViewSize.Y, 8),
			1));

	static bool bLoggedApplyScheduled = false;
	if (!bLoggedApplyScheduled)
	{
		UE_LOG(LogTemp, Warning, TEXT("DDGI DIAG: apply compute pass scheduled. Probes=%u RaysPerProbe=%u HashCapacity=%u View=%dx%d OverrideOutput=%d."),
			ProbeCount, TotalRaysPerProbe, HashCapacity, ViewSize.X, ViewSize.Y,
			Inputs.OverrideOutput.IsValid() ? 1 : 0);
		bLoggedApplyScheduled = true;
	}

	// SceneViewExtension callbacks must write to OverrideOutput when the
	// post-process graph supplies one (typically when this is the final pass).
	// The compute shader writes to a UAV-capable intermediate texture; copy its
	// viewport region into the engine-owned output target before returning it.
	if (Inputs.OverrideOutput.IsValid())
	{
		AddDrawTexturePass(
			GraphBuilder,
			static_cast<const FViewInfo&>(View),
			OutputTexture,
			Inputs.OverrideOutput.Texture,
			SceneColor.ViewRect.Min,
			Inputs.OverrideOutput.ViewRect.Min,
			SceneColor.ViewRect.Size());
		return FScreenPassTexture(
			Inputs.OverrideOutput.Texture,
			Inputs.OverrideOutput.ViewRect);
	}

	return FScreenPassTexture(OutputTexture, SceneColor.ViewRect);
}

FDDGIRadianceCascadeRHIViewExtension::FDDGIRadianceCascadeRHIViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
}

bool FDDGIRadianceCascadeRHIViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	return true;
}

void FDDGIRadianceCascadeRHIViewExtension::SubscribeToPostProcessingPass(
	EPostProcessingPass Pass,
	FAfterPassCallbackDelegateArray& InOutPassCallbacks,
	bool bIsPassEnabled)
{
	if (Pass == EPostProcessingPass::MotionBlur)
	{
		InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateStatic(
			&DDGIApplyRadianceCascadesAfterMotionBlur));
		static bool bLoggedSubscription = false;
		if (!bLoggedSubscription)
		{
			UE_LOG(LogTemp, Warning, TEXT("DDGI DIAG: subscribed to MotionBlur post-process pass (enabled=%d)."), bIsPassEnabled ? 1 : 0);
			bLoggedSubscription = true;
		}
	}
}

void FDDGIRadianceCascadeRHIViewExtension::PreRenderView_RenderThread(
	FRHICommandListImmediate& RHICmdList,
	FSceneView& InView)
{
	check(IsInRenderingThread());
	static bool bLoggedPreRenderEntry = false;
	if (!bLoggedPreRenderEntry)
	{
		UE_LOG(LogTemp, Warning, TEXT("DDGI DIAG: PreRenderView_RenderThread executed."));
		bLoggedPreRenderEntry = true;
	}

	if (!InView.Family || !InView.Family->Scene)
	{
		return;
	}

	const FSceneInterface* Scene = InView.Family->Scene;
	const FDDGIMasterConfig* Config = DDGIGetMasterConfig_RenderThread(Scene);
	if (!Config || !Config->bEnableDynamicGI)
	{
		static bool bLoggedPreRenderNoConfig = false;
		if (!bLoggedPreRenderNoConfig)
		{
			UE_LOG(LogTemp, Warning, TEXT("DDGI DIAG: PreRenderView has no enabled master config for this scene."));
			bLoggedPreRenderNoConfig = true;
		}
		return;
	}

	// Probe positions are anchored to world origin. The active view controls
	// when this scene-wide update runs, but never translates the probe lattice.
	if (!Config->SoftwareBVH.IsValid())
	{
		return;
	}

	FShaderResourceViewRHIRef NodesSRV;
	FShaderResourceViewRHIRef TrianglesSRV;
	uint32 NodeCount = 0;
	uint32 TriangleCount = 0;
	if (!DDGIGetSoftwareBVHResources_RenderThread(Scene, NodesSRV, TrianglesSRV, NodeCount, TriangleCount))
	{
		// The resource manager supplies valid zero-count buffers for an empty BVH.
		return;
	}

	FTexture2DRHIRef DirectionsTexture;
	FTexture2DRHIRef RadianceTexture;
	FTexture2DRHIRef HitDistanceTexture;
	FTexture2DRHIRef HitTriangleIndexTexture;
	FTexture2DRHIRef ProbePositionsTexture;
	uint32 ProbeCount = 0;
	uint32 TotalRaysPerProbe = 0;
	if (!DDGIGetRadianceCascadeTracingRHIResources_RenderThread(
		Scene, DirectionsTexture, RadianceTexture, HitDistanceTexture, HitTriangleIndexTexture, ProbePositionsTexture,
		ProbeCount, TotalRaysPerProbe))
	{
		static bool bLoggedPreRenderNoResources = false;
		if (!bLoggedPreRenderNoResources)
		{
			UE_LOG(LogTemp, Error, TEXT("DDGI DIAG: PreRenderView cannot acquire tracing GPU resources."));
			bLoggedPreRenderNoResources = true;
		}
		return;
	}

	static bool bLoggedTraceStart = false;
	if (!bLoggedTraceStart)
	{
		UE_LOG(LogTemp, Warning, TEXT("DDGI DIAG: tracing resources acquired. Probes=%u RaysPerProbe=%u."), ProbeCount, TotalRaysPerProbe);
		bLoggedTraceStart = true;
	}

	const uint32 TotalTexels = ProbeCount * TotalRaysPerProbe;
	if (TotalTexels == 0)
	{
		return;
	}

	// Prepare every resource required by Trace, Gather and Integrate before
	// dispatching anything. A late allocation failure must not leave Radiance
	// partially overwritten by Trace while Apply still reads the previous frame.
	FTexture2DRHIRef RadianceScratchTexture;
	FTexture2DRHIRef RadianceIntegrationScratchTexture;
	FShaderResourceViewRHIRef SpatialHashSRV, NeighborListSRV;
	uint32 HashCapacity = 0, SpatialProbeCount = 0, NeighborListProbeCount = 0;
	float ProbeSpacing = 0.0f;
	const bool bRequestNeighborList = CVarDDGIUsePrecomputedProbeNeighbors.GetValueOnRenderThread() != 0;
	uint32 bUseNeighborList = 0u;
	if (bRequestNeighborList)
	{
		bUseNeighborList =
			DDGIGetRadianceCascadeProbeNeighborList_RenderThread(Scene, NeighborListSRV, NeighborListProbeCount) &&
			NeighborListSRV.IsValid() && NeighborListProbeCount == ProbeCount
			? 1u : 0u;
	}
	if (!DDGIGetRadianceCascadeScratchTexture_RenderThread(Scene, RadianceScratchTexture) ||
		!DDGIGetRadianceCascadeIntegrationScratchTexture_RenderThread(Scene, RadianceIntegrationScratchTexture) ||
		!DDGIGetRadianceCascadeProbePositionsRHI_RenderThread(Scene, ProbePositionsTexture, SpatialProbeCount) ||
		SpatialProbeCount != ProbeCount)
	{
		return;
	}
	if (bUseNeighborList)
	{
		ProbeSpacing = FMath::Max(1.0f, Config->ProbeSpacing);
	}
	else
	{
		const bool bHasSpatialHash = DDGIGetRadianceCascadeProbeSpatialHash_RenderThread(
			Scene, SpatialHashSRV, HashCapacity, ProbeSpacing);
		if (!bHasSpatialHash || !SpatialHashSRV.IsValid() || HashCapacity == 0)
		{
			return;
		}
	}
	FShaderResourceViewRHIRef GatherRadianceSRV = RHICreateShaderResourceView(RadianceTexture, 0);
	FShaderResourceViewRHIRef GatherHitSRV = RHICreateShaderResourceView(HitDistanceTexture, 0);
	FShaderResourceViewRHIRef GatherHitIndicesSRV = RHICreateShaderResourceView(HitTriangleIndexTexture, 0);
	FShaderResourceViewRHIRef GatherDirectionsSRV = RHICreateShaderResourceView(DirectionsTexture, 0);
	FShaderResourceViewRHIRef GatherPositionsSRV = RHICreateShaderResourceView(ProbePositionsTexture, 0);
	FUnorderedAccessViewRHIRef GatherOutputUAV = RHICreateUnorderedAccessView(RadianceScratchTexture);
	FShaderResourceViewRHIRef GatherOutputSRV = RHICreateShaderResourceView(RadianceScratchTexture, 0);
	FShaderResourceViewRHIRef IntegrationScratchSRV = RHICreateShaderResourceView(RadianceIntegrationScratchTexture, 0);
	FUnorderedAccessViewRHIRef IntegrationScratchUAV = RHICreateUnorderedAccessView(RadianceIntegrationScratchTexture);
	FShaderResourceViewRHIRef IntegrationHitDistanceSRV = RHICreateShaderResourceView(HitDistanceTexture, 0);
	if (!GatherRadianceSRV.IsValid() || !GatherHitSRV.IsValid() || !GatherHitIndicesSRV.IsValid() ||
		!GatherDirectionsSRV.IsValid() || !GatherPositionsSRV.IsValid() || !TrianglesSRV.IsValid() ||
		!GatherOutputUAV.IsValid() || !GatherOutputSRV.IsValid() ||
		!IntegrationScratchSRV.IsValid() || !IntegrationScratchUAV.IsValid() ||
		!IntegrationHitDistanceSRV.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to prepare Gather/Integrate SRV/UAV views."));
		return;
	}

	FShaderResourceViewRHIRef DirectionsSRV = RHICreateShaderResourceView(DirectionsTexture, 0);
	FShaderResourceViewRHIRef LogicalToPhysicalSRV, NewPhysicalSlotsSRV;
	uint32 NewPhysicalSlotCount = 0, PhysicalSlotCapacity = 0;
	if (!DDGIGetRadianceCascadePhysicalSlotResources_RenderThread(
		Scene, LogicalToPhysicalSRV, NewPhysicalSlotsSRV, NewPhysicalSlotCount, PhysicalSlotCapacity) ||
		!LogicalToPhysicalSRV.IsValid() || PhysicalSlotCapacity == 0u)
	{
		UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to acquire logical-to-physical probe mapping."));
		return;
	}
	FShaderResourceViewRHIRef ProbePositionsSRV = RHICreateShaderResourceView(ProbePositionsTexture, 0);
	FUnorderedAccessViewRHIRef RadianceUAV = RHICreateUnorderedAccessView(RadianceTexture);
	FUnorderedAccessViewRHIRef HitDistanceUAV = RHICreateUnorderedAccessView(HitDistanceTexture);
	FUnorderedAccessViewRHIRef HitTriangleIndexUAV = RHICreateUnorderedAccessView(HitTriangleIndexTexture);
	if (!DirectionsSRV.IsValid() || !ProbePositionsSRV.IsValid() || !RadianceUAV.IsValid() || !HitDistanceUAV.IsValid() || !HitTriangleIndexUAV.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to create tracing SRV/UAV views."));
		return;
	}

	// Reserve the frame immediately before the first GPU dispatch. If initialization
	// is pending, its UAVs must be valid first; otherwise tracing views are checked
	// before the frame is claimed.
	bool bClaimedUpdateFrame = false;

	const uint32 TotalGroups = FMath::DivideAndRoundUp(TotalTexels, 64u);
	const uint32 GroupsX = FMath::Min(TotalGroups, 65535u);
	const uint32 GroupsY = FMath::DivideAndRoundUp(TotalGroups, GroupsX);

	// Initialize shared ray directions when their texture is created, and clear
	// history only for newly allocated or explicitly invalidated physical slots.
	// Keep initialization outputs separate from the tracing resources.
	// The initialization getter SafeReleases its output references when no
	// initialization is pending; sharing the tracing references would null them.
	FTexture2DRHIRef InitDirectionsTexture;
	FTexture2DRHIRef InitRadianceTexture;
	FTexture2DRHIRef InitHitDistanceTexture;
	FTexture2DRHIRef InitRadianceScratchTexture;
	FTexture2DRHIRef InitRadianceIntegrationScratchTexture;
	FTexture2DRHIRef InitHitTriangleIndexTexture;
	uint32 InitPendingSlotCount = 0;
	bool bInitializeDirections = false;
	if (DDGIGetRadianceCascadeInitializationRHIResources_RenderThread(
		Scene, InitDirectionsTexture, InitRadianceTexture, InitHitDistanceTexture,
		InitRadianceScratchTexture, InitRadianceIntegrationScratchTexture,
		InitHitTriangleIndexTexture, InitPendingSlotCount, bInitializeDirections))
	{
		if (!InitDirectionsTexture.IsValid() ||
			!InitRadianceTexture.IsValid() ||
			!InitHitDistanceTexture.IsValid() ||
			!InitRadianceScratchTexture.IsValid() ||
			!InitRadianceIntegrationScratchTexture.IsValid() ||
			!InitHitTriangleIndexTexture.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: initialization returned invalid textures."));
			return;
		}

		FUnorderedAccessViewRHIRef InitDirectionsUAV = RHICreateUnorderedAccessView(InitDirectionsTexture);
		FUnorderedAccessViewRHIRef InitRadianceUAV = RHICreateUnorderedAccessView(InitRadianceTexture);
		FUnorderedAccessViewRHIRef InitHitDistanceUAV = RHICreateUnorderedAccessView(InitHitDistanceTexture);
		FUnorderedAccessViewRHIRef InitRadianceScratchUAV = RHICreateUnorderedAccessView(InitRadianceScratchTexture);
		FUnorderedAccessViewRHIRef InitRadianceIntegrationScratchUAV = RHICreateUnorderedAccessView(InitRadianceIntegrationScratchTexture);
		FUnorderedAccessViewRHIRef InitHitTriangleIndexUAV = RHICreateUnorderedAccessView(InitHitTriangleIndexTexture);

		if (InitDirectionsUAV.IsValid() && InitRadianceUAV.IsValid() && InitHitDistanceUAV.IsValid() &&
			InitRadianceScratchUAV.IsValid() && InitRadianceIntegrationScratchUAV.IsValid() &&
			InitHitTriangleIndexUAV.IsValid())
		{
			if (!DDGIBeginRadianceCascadeUpdate_RenderThread(Scene, InView.Family->FrameNumber))
			{
				return;
			}
			bClaimedUpdateFrame = true;

			DDGITransitionTexture(RHICmdList, InitDirectionsTexture, ERHIAccess::Unknown, ERHIAccess::UAVCompute);
			DDGITransitionTexture(RHICmdList, InitRadianceTexture, ERHIAccess::Unknown, ERHIAccess::UAVCompute);
			DDGITransitionTexture(RHICmdList, InitHitDistanceTexture, ERHIAccess::Unknown, ERHIAccess::UAVCompute);
			DDGITransitionTexture(RHICmdList, InitRadianceScratchTexture, ERHIAccess::Unknown, ERHIAccess::UAVCompute);
			DDGITransitionTexture(RHICmdList, InitRadianceIntegrationScratchTexture, ERHIAccess::Unknown, ERHIAccess::UAVCompute);
			DDGITransitionTexture(RHICmdList, InitHitTriangleIndexTexture, ERHIAccess::Unknown, ERHIAccess::UAVCompute);

			TShaderMapRef<FDDGIRadianceCascadeInitializeRHI> InitializeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
			RHICmdList.SetComputeShader(InitializeShader.GetComputeShader());
			const uint32 InitTotalTexels = InitPendingSlotCount * TotalRaysPerProbe;
			const uint32 InitTotalGroups = FMath::DivideAndRoundUp(InitTotalTexels, 64u);
			const uint32 InitGroupsX = FMath::Max(1u, FMath::Min(InitTotalGroups, 65535u));
			const uint32 InitGroupsY = FMath::DivideAndRoundUp(InitTotalGroups, InitGroupsX);
			InitializeShader->SetParameters(
				RHICmdList, InitializeShader.GetComputeShader(), InitDirectionsUAV, InitRadianceUAV,
				InitHitDistanceUAV, InitRadianceScratchUAV, InitRadianceIntegrationScratchUAV,
				InitHitTriangleIndexUAV, NewPhysicalSlotsSRV, bInitializeDirections ? 1u : 0u,
				TotalRaysPerProbe,
				static_cast<uint32>(Config->RadianceCascades.GetSafeCascadeCount()),
				static_cast<uint32>(FMath::Clamp(Config->RadianceCascades.BaseRayCount, 1, 64)),
				static_cast<uint32>(FMath::Clamp(Config->RadianceCascades.RaySplitFactor, 1, 4)),
				InitPendingSlotCount, InitTotalTexels, InitGroupsX);
			DispatchComputeShader(RHICmdList, InitializeShader.GetShader(), InitGroupsX, InitGroupsY, 1);
			InitializeShader->UnbindResources(RHICmdList, InitializeShader.GetComputeShader());

			DDGITransitionTexture(RHICmdList, InitDirectionsTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);
			DDGITransitionTexture(RHICmdList, InitRadianceTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);
			DDGITransitionTexture(RHICmdList, InitHitDistanceTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);
			DDGITransitionTexture(RHICmdList, InitRadianceScratchTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);
			DDGITransitionTexture(RHICmdList, InitRadianceIntegrationScratchTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);
			DDGITransitionTexture(RHICmdList, InitHitTriangleIndexTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);
			DDGIMarkRadianceCascadeInitializationScheduled_RenderThread(Scene);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to create initialization UAVs."));
			return;
		}
	}


	// With no initialization dispatch this is the first GPU operation, so only
	// claim after all tracing SRV/UAV views have been created successfully.
	if (!bClaimedUpdateFrame &&
		!DDGIBeginRadianceCascadeUpdate_RenderThread(Scene, InView.Family->FrameNumber))
	{
		return;
	}

	DDGITransitionTexture(RHICmdList, DirectionsTexture, ERHIAccess::Unknown, ERHIAccess::SRVCompute);
	DDGITransitionTexture(RHICmdList, ProbePositionsTexture, ERHIAccess::Unknown, ERHIAccess::SRVCompute);
	DDGITransitionTexture(RHICmdList, RadianceTexture, ERHIAccess::Unknown, ERHIAccess::UAVCompute);
	DDGITransitionTexture(RHICmdList, HitDistanceTexture, ERHIAccess::Unknown, ERHIAccess::UAVCompute);
	DDGITransitionTexture(RHICmdList, HitTriangleIndexTexture, ERHIAccess::Unknown, ERHIAccess::UAVCompute);

	static bool bLoggedTraceLightingConfig = false;
	if (!bLoggedTraceLightingConfig)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("DDGI DIAG: GPU tracing lighting config: HasDirectionalLight=%d Direction=(%.4f, %.4f, %.4f) Color=(%.4f, %.4f, %.4f) Intensity=%.6f; StaticBackground=%d Color=(%.4f, %.4f, %.4f) Intensity=%.4f Transition=(%.1f, %.1f); GIIntensity=%.4f."),
			Config->bHasDirectionalLight ? 1 : 0,
			Config->DirectionalLightDirection.X, Config->DirectionalLightDirection.Y, Config->DirectionalLightDirection.Z,
			Config->DirectionalLightColor.R, Config->DirectionalLightColor.G, Config->DirectionalLightColor.B,
			Config->DirectionalLightIntensity,
			Config->bEnableStaticBackground ? 1 : 0,
			Config->StaticBackgroundColor.R, Config->StaticBackgroundColor.G, Config->StaticBackgroundColor.B,
			Config->StaticBackgroundIntensity,
			Config->BackgroundTransitionStart, Config->BackgroundTransitionEnd,
			Config->GIIntensity);
		bLoggedTraceLightingConfig = true;
	}

	TShaderMapRef<FDDGIRadianceCascadeTraceRHI> TraceShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
	RHICmdList.SetComputeShader(TraceShader.GetComputeShader());
	TraceShader->SetParameters(
		RHICmdList, TraceShader.GetComputeShader(), DirectionsSRV, ProbePositionsSRV, LogicalToPhysicalSRV, NodesSRV, TrianglesSRV,
		RadianceUAV, HitDistanceUAV, HitTriangleIndexUAV,
		TotalTexels, GroupsX, ProbeCount, TotalRaysPerProbe,
		NodeCount, TriangleCount,
		static_cast<uint32>(Config->RadianceCascades.GetSafeCascadeCount()),
		Config->RadianceCascades.BaseSegmentLength, Config->RadianceCascades.CascadeScale,
		Config->RadianceCascades.MaxTraceDistance,
		FVector4(Config->StaticBackgroundColor.R, Config->StaticBackgroundColor.G, Config->StaticBackgroundColor.B, Config->StaticBackgroundColor.A),
		Config->StaticBackgroundIntensity, Config->GIIntensity,
		FVector4(Config->ProbeGridOrigin.X, Config->ProbeGridOrigin.Y, Config->ProbeGridOrigin.Z, 0.0f),
		FVector4(
			InView.ViewMatrices.GetViewOrigin().X - Config->SoftwareBVH->WorldOrigin.X,
			InView.ViewMatrices.GetViewOrigin().Y - Config->SoftwareBVH->WorldOrigin.Y,
			InView.ViewMatrices.GetViewOrigin().Z - Config->SoftwareBVH->WorldOrigin.Z, 0.0f),
		Config->BackgroundTransitionStart, Config->BackgroundTransitionEnd,
		Config->bEnableStaticBackground ? 1u : 0u,
		FVector4(Config->DirectionalLightDirection.X, Config->DirectionalLightDirection.Y, Config->DirectionalLightDirection.Z, 0.0f),
		FVector4(Config->DirectionalLightColor.R, Config->DirectionalLightColor.G, Config->DirectionalLightColor.B, 0.0f),
		Config->DirectionalLightIntensity,
		Config->bHasDirectionalLight ? 1u : 0u);
	DispatchComputeShader(RHICmdList, TraceShader.GetShader(), GroupsX, GroupsY, 1);
	TraceShader->UnbindResources(RHICmdList, TraceShader.GetComputeShader());

	DDGITransitionTexture(RHICmdList, RadianceTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);
	DDGITransitionTexture(RHICmdList, HitDistanceTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);
	DDGITransitionTexture(RHICmdList, HitTriangleIndexTexture, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);

	DDGITransitionTexture(RHICmdList, RadianceScratchTexture, ERHIAccess::Unknown, ERHIAccess::UAVCompute);
	TShaderMapRef<FDDGIRadianceCascadeSpatialGatherRHI> GatherShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
	RHICmdList.SetComputeShader(GatherShader.GetComputeShader());
	GatherShader->SetParameters(RHICmdList,GatherShader.GetComputeShader(),GatherRadianceSRV,GatherHitSRV,GatherHitIndicesSRV,GatherDirectionsSRV,
		GatherPositionsSRV,LogicalToPhysicalSRV,TrianglesSRV,SpatialHashSRV,NeighborListSRV,GatherOutputUAV,TotalTexels,GroupsX,ProbeCount,
		TotalRaysPerProbe,HashCapacity,TriangleCount,ProbeSpacing,
		FVector4(Config->ProbeGridOrigin.X,Config->ProbeGridOrigin.Y,Config->ProbeGridOrigin.Z,0.0f),bUseNeighborList);
	DispatchComputeShader(RHICmdList,GatherShader.GetShader(),GroupsX,GroupsY,1);
	GatherShader->UnbindResources(RHICmdList,GatherShader.GetComputeShader());
	

	// Gather output remains immutable during integration. Each dispatch reads
	// its child cascade from the previous result and writes only the parent range
	// into the alternate output texture. The first cascade is the only result
	// consumed by deferred lighting.
	{
		const uint32 CascadeCount = static_cast<uint32>(Config->RadianceCascades.GetSafeCascadeCount());
		const uint32 TotalRays = static_cast<uint32>(Config->TotalRaysPerProbe);
		TShaderMapRef<FDDGIRadianceCascadeIntegrateRHI> IntegrateShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));

		FShaderResourceViewRHIRef ChildRadianceSRV = GatherOutputSRV;
		FTexture2DRHIRef IntegrationDestinationTexture = RadianceTexture;
		FUnorderedAccessViewRHIRef IntegrationDestinationUAV = RadianceUAV;
		bool bIntegrationDestinationIsPrimary = true;
		bool bFinalRadianceIsPrimary = false;
		bool bDispatchedIntegration = false;

		DDGITransitionTexture(RHICmdList, RadianceScratchTexture,
			ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);

		for (int32 CascadeIndex = static_cast<int32>(CascadeCount) - 2; CascadeIndex >= 0; --CascadeIndex)
		{
			const uint32 CurrentCascadeRayCount = static_cast<uint32>(
				Config->RadianceCascades.GetRayCount(CascadeIndex));
			const uint32 IntegrationTexels = ProbeCount * CurrentCascadeRayCount;
			if (IntegrationTexels == 0u)
			{
				continue;
			}

			const uint32 IntegrationGroups = FMath::Min(
				FMath::DivideAndRoundUp(IntegrationTexels, 64u), 65535u);
			const uint32 IntegrationGroupsY = FMath::DivideAndRoundUp(
				IntegrationTexels, IntegrationGroups * 64u);

			DDGITransitionTexture(RHICmdList, IntegrationDestinationTexture,
				ERHIAccess::SRVCompute, ERHIAccess::UAVCompute);

			RHICmdList.SetComputeShader(IntegrateShader.GetComputeShader());
			IntegrateShader->SetParameters(
				RHICmdList, IntegrateShader.GetComputeShader(),
				IntegrationDestinationUAV, GatherOutputSRV, ChildRadianceSRV,
				IntegrationHitDistanceSRV, LogicalToPhysicalSRV,
				IntegrationTexels, IntegrationGroups, TotalRays,
				static_cast<uint32>(Config->RadianceCascades.GetRayStorageOffset(CascadeIndex)),
				CurrentCascadeRayCount,
				static_cast<uint32>(Config->RadianceCascades.GetRayStorageOffset(CascadeIndex + 1)),
				static_cast<uint32>(FMath::Max(1,
					Config->RadianceCascades.GetRayCount(CascadeIndex + 1) /
					FMath::Max(1, Config->RadianceCascades.GetRayCount(CascadeIndex)))));
			DispatchComputeShader(RHICmdList, IntegrateShader.GetShader(), IntegrationGroups, IntegrationGroupsY, 1);
			IntegrateShader->UnbindResources(RHICmdList, IntegrateShader.GetComputeShader());

			DDGITransitionTexture(RHICmdList, IntegrationDestinationTexture,
				ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);
			bFinalRadianceIsPrimary = bIntegrationDestinationIsPrimary;
			bDispatchedIntegration = true;

			ChildRadianceSRV = bIntegrationDestinationIsPrimary
				? GatherRadianceSRV
				: IntegrationScratchSRV;

			if (bIntegrationDestinationIsPrimary)
			{
				IntegrationDestinationTexture = RadianceIntegrationScratchTexture;
				IntegrationDestinationUAV = IntegrationScratchUAV;
				bIntegrationDestinationIsPrimary = false;
			}
			else
			{
				IntegrationDestinationTexture = RadianceTexture;
				IntegrationDestinationUAV = RadianceUAV;
				bIntegrationDestinationIsPrimary = true;
			}
		}

		if (!bDispatchedIntegration)
		{
			// With one cascade, Gather already produced the complete result.
			DDGISwapRadianceCascadeRadianceBuffers_RenderThread(Scene);
		}
		else if (!bFinalRadianceIsPrimary)
		{
			DDGISwapRadianceCascadeIntegrationBuffers_RenderThread(Scene);
		}
	}

}
