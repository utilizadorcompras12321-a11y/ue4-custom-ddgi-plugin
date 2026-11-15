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
    0,
    TEXT("Use precomputed probe neighbors in DDGI spatial gather (0=spatial hash, 1=precomputed list)."),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarDDGIDebugApply(
    TEXT("r.DDGI.DebugApply"),
    0,
    TEXT("DDGI apply debug view (0=off, 1=probe coverage, 2=raw irradiance, 3=solid magenta output test)."),
    ECVF_RenderThreadSafe);

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
		BackgroundTransitionStart.Bind(Initializer.ParameterMap, TEXT("BackgroundTransitionStart"));
		BackgroundTransitionEnd.Bind(Initializer.ParameterMap, TEXT("BackgroundTransitionEnd"));
		bEnableStaticBackground.Bind(Initializer.ParameterMap, TEXT("bEnableStaticBackground"));

		DirectionalLightDirection.Bind(Initializer.ParameterMap, TEXT("DirectionalLightDirection"));
		DirectionalLightColor.Bind(Initializer.ParameterMap, TEXT("DirectionalLightColor"));
		DirectionalLightIntensity.Bind(Initializer.ParameterMap, TEXT("DirectionalLightIntensity"));
		bHasDirectionalLight.Bind(Initializer.ParameterMap, TEXT("bHasDirectionalLight"));
		DirectionsTexture.Bind(Initializer.ParameterMap, TEXT("DirectionsTexture"));
		ProbePositionsTexture.Bind(Initializer.ParameterMap, TEXT("ProbePositionsTexture"));
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
		SetShaderValue(RHICmdList, ShaderRHI, BackgroundTransitionStart, InBackgroundTransitionStart);
		SetShaderValue(RHICmdList, ShaderRHI, BackgroundTransitionEnd, InBackgroundTransitionEnd);
		SetShaderValue(RHICmdList, ShaderRHI, bEnableStaticBackground, bInEnableStaticBackground);

		SetShaderValue(RHICmdList, ShaderRHI, DirectionalLightDirection, InDirectionalLightDirection);
		SetShaderValue(RHICmdList, ShaderRHI, DirectionalLightColor, InDirectionalLightColor);
		SetShaderValue(RHICmdList, ShaderRHI, DirectionalLightIntensity, InDirectionalLightIntensity);
		SetShaderValue(RHICmdList, ShaderRHI, bHasDirectionalLight, bInHasDirectionalLight);

		if (DirectionsTexture.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, DirectionsTexture.GetBaseIndex(), DirectionsSRV);
		if (ProbePositionsTexture.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, ProbePositionsTexture.GetBaseIndex(), ProbePositionsSRV);
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
	LAYOUT_FIELD(FShaderParameter, BackgroundTransitionStart);
	LAYOUT_FIELD(FShaderParameter, BackgroundTransitionEnd);
	LAYOUT_FIELD(FShaderParameter, bEnableStaticBackground);

	LAYOUT_FIELD(FShaderParameter, DirectionalLightDirection);
	LAYOUT_FIELD(FShaderParameter, DirectionalLightColor);
	LAYOUT_FIELD(FShaderParameter, DirectionalLightIntensity);
	LAYOUT_FIELD(FShaderParameter, bHasDirectionalLight);
	LAYOUT_FIELD(FShaderResourceParameter, DirectionsTexture);
	LAYOUT_FIELD(FShaderResourceParameter, ProbePositionsTexture);
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
 }
 static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P){return IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM5);}
 template<typename TShaderRHIParamRef>
 void SetParameters(FRHICommandList& C,const TShaderRHIParamRef Sh,FShaderResourceViewRHIRef R,FShaderResourceViewRHIRef H,FShaderResourceViewRHIRef HitIndices,
  FShaderResourceViewRHIRef D,FShaderResourceViewRHIRef P,FShaderResourceViewRHIRef Triangles,FShaderResourceViewRHIRef S,
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
		TotalTexels.Bind(Initializer.ParameterMap, TEXT("TotalTexels"));
		DispatchGroupsX.Bind(Initializer.ParameterMap, TEXT("DispatchGroupsX"));
		TotalRaysPerProbe.Bind(Initializer.ParameterMap, TEXT("TotalRaysPerProbe"));
		CascadeStorageOffset.Bind(Initializer.ParameterMap, TEXT("CascadeStorageOffset"));
		CascadeRayCount.Bind(Initializer.ParameterMap, TEXT("CascadeRayCount"));
		NextCascadeStorageOffset.Bind(Initializer.ParameterMap, TEXT("NextCascadeStorageOffset"));
		EffectiveSplitFactor.Bind(Initializer.ParameterMap, TEXT("EffectiveSplitFactor"));
		InputRadianceTexture.Bind(Initializer.ParameterMap, TEXT("InputRadianceTexture"));
		HitDistanceTexture.Bind(Initializer.ParameterMap, TEXT("HitDistanceTexture"));
		OutputRadianceUAV.Bind(Initializer.ParameterMap, TEXT("OutputRadianceUAV"));
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	template<typename TShaderRHIParamRef>
	void SetParameters(
		FRHICommandList& RHICmdList,
		const TShaderRHIParamRef ShaderRHI,
		FShaderResourceViewRHIRef InputRadianceSRV,
		FShaderResourceViewRHIRef HitDistanceSRV,
		FUnorderedAccessViewRHIRef OutputRadianceView,
		uint32 InTotalTexels,
		uint32 InDispatchGroupsX,
		uint32 InTotalRaysPerProbe,
		uint32 InCascadeStorageOffset,
		uint32 InCascadeRayCount,
		uint32 InNextCascadeStorageOffset,
		uint32 InEffectiveSplitFactor)
	{
		const uint32 InTextureWidth = 4096u;
		SetShaderValue(RHICmdList, ShaderRHI, TextureWidth, InTextureWidth);
		SetShaderValue(RHICmdList, ShaderRHI, TotalTexels, InTotalTexels);
		SetShaderValue(RHICmdList, ShaderRHI, DispatchGroupsX, InDispatchGroupsX);
		SetShaderValue(RHICmdList, ShaderRHI, TotalRaysPerProbe, InTotalRaysPerProbe);
		SetShaderValue(RHICmdList, ShaderRHI, CascadeStorageOffset, InCascadeStorageOffset);
		SetShaderValue(RHICmdList, ShaderRHI, CascadeRayCount, InCascadeRayCount);
		SetShaderValue(RHICmdList, ShaderRHI, NextCascadeStorageOffset, InNextCascadeStorageOffset);
		SetShaderValue(RHICmdList, ShaderRHI, EffectiveSplitFactor, InEffectiveSplitFactor);
		if (InputRadianceTexture.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, InputRadianceTexture.GetBaseIndex(), InputRadianceSRV);
		}
		if (HitDistanceTexture.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, HitDistanceTexture.GetBaseIndex(), HitDistanceSRV);
		}
		if (OutputRadianceUAV.IsBound())
		{
			RHICmdList.SetUAVParameter(ShaderRHI, OutputRadianceUAV.GetBaseIndex(), OutputRadianceView);
		}
	}

	template<typename TShaderRHIParamRef>
	void UnbindResources(FRHICommandList& RHICmdList, const TShaderRHIParamRef ShaderRHI)
	{
		if (InputRadianceTexture.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, InputRadianceTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
		}
		if (HitDistanceTexture.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, HitDistanceTexture.GetBaseIndex(), FShaderResourceViewRHIRef());
		}
		if (OutputRadianceUAV.IsBound())
		{
			RHICmdList.SetUAVParameter(ShaderRHI, OutputRadianceUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
		}
	}

private:
	LAYOUT_FIELD(FShaderParameter, TextureWidth);
	LAYOUT_FIELD(FShaderParameter, TotalTexels);
	LAYOUT_FIELD(FShaderParameter, DispatchGroupsX);
	LAYOUT_FIELD(FShaderParameter, TotalRaysPerProbe);
	LAYOUT_FIELD(FShaderParameter, CascadeStorageOffset);
	LAYOUT_FIELD(FShaderParameter, CascadeRayCount);
	LAYOUT_FIELD(FShaderParameter, NextCascadeStorageOffset);
	LAYOUT_FIELD(FShaderParameter, EffectiveSplitFactor);
	LAYOUT_FIELD(FShaderResourceParameter, InputRadianceTexture);
	LAYOUT_FIELD(FShaderResourceParameter, HitDistanceTexture);
	LAYOUT_FIELD(FShaderResourceParameter, OutputRadianceUAV);
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
		SHADER_PARAMETER_SRV(StructuredBuffer<float4>, ProbeSpatialHash)
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
		SHADER_PARAMETER(FVector4, BVHWorldOrigin)
		SHADER_PARAMETER(uint32, ShouldUsePreExposure)
		SHADER_PARAMETER(uint32, DebugMode)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

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
	uint32 HashCapacity = 0;
	float ProbeSpacing = 0.0f;
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
	PassParameters->BVHWorldOrigin = FVector4(
		Config->SoftwareBVH->WorldOrigin.X,
		Config->SoftwareBVH->WorldOrigin.Y,
		Config->SoftwareBVH->WorldOrigin.Z, 0.0f);
	PassParameters->ShouldUsePreExposure = 1u;
	PassParameters->DebugMode = static_cast<uint32>(FMath::Clamp(CVarDDGIDebugApply.GetValueOnRenderThread(), 0, 3));

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

	// Probe data is scene-wide. The first view that claims this frame also
	// supplies the camera center; later views must not move the grid again.
	if (!DDGIBeginRadianceCascadeUpdate_RenderThread(Scene, InView.Family->FrameNumber))
	{
		return;
	}

	DDGIUpdateCameraProbeGridOrigin_RenderThread(
		Scene, InView.ViewMatrices.GetViewOrigin());
	Config = DDGIGetMasterConfig_RenderThread(Scene);
	if (!Config || !Config->SoftwareBVH.IsValid())
	{
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

	const uint32 TotalGroups = FMath::DivideAndRoundUp(TotalTexels, 64u);
	const uint32 GroupsX = FMath::Min(TotalGroups, 65535u);
	const uint32 GroupsY = FMath::DivideAndRoundUp(TotalGroups, GroupsX);

	// Initialize ray directions and clear radiance when the probe layout or snapped
	// camera-centered grid changes, or when GPU storage is recreated.
	// Keep initialization outputs separate from the tracing resources.
	// The initialization getter SafeReleases its output references when no
	// initialization is pending; sharing the tracing references would null them.
	FTexture2DRHIRef InitDirectionsTexture;
	FTexture2DRHIRef InitRadianceTexture;
	FTexture2DRHIRef InitHitDistanceTexture;
	uint32 InitProbeCount = 0;
	if (DDGIGetRadianceCascadeInitializationRHIResources_RenderThread(
		Scene, InitDirectionsTexture, InitRadianceTexture, InitHitDistanceTexture, InitProbeCount))
	{
		if (!InitDirectionsTexture.IsValid() ||
			!InitRadianceTexture.IsValid() ||
			!InitHitDistanceTexture.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: initialization returned invalid textures."));
			return;
		}

		FUnorderedAccessViewRHIRef InitDirectionsUAV = RHICreateUnorderedAccessView(InitDirectionsTexture);
		FUnorderedAccessViewRHIRef InitRadianceUAV = RHICreateUnorderedAccessView(InitRadianceTexture);
		FUnorderedAccessViewRHIRef InitHitDistanceUAV = RHICreateUnorderedAccessView(InitHitDistanceTexture);

		if (InitDirectionsUAV.IsValid() && InitRadianceUAV.IsValid() && InitHitDistanceUAV.IsValid())
		{
			RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, InitDirectionsTexture);
			RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, InitRadianceTexture);
			RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, InitHitDistanceTexture);

			TShaderMapRef<FDDGIRadianceCascadeInitializeRHI> InitializeShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
			RHICmdList.SetComputeShader(InitializeShader.GetComputeShader());
			InitializeShader->SetParameters(
				RHICmdList, InitializeShader.GetComputeShader(), InitDirectionsUAV, InitRadianceUAV, InitHitDistanceUAV,
				TotalRaysPerProbe,
				static_cast<uint32>(Config->RadianceCascades.GetSafeCascadeCount()),
				static_cast<uint32>(FMath::Clamp(Config->RadianceCascades.BaseRayCount, 1, 64)),
				static_cast<uint32>(FMath::Clamp(Config->RadianceCascades.RaySplitFactor, 1, 4)),
				InitProbeCount, TotalTexels, GroupsX);
			DispatchComputeShader(RHICmdList, InitializeShader.GetShader(), GroupsX, GroupsY, 1);
			InitializeShader->UnbindResources(RHICmdList, InitializeShader.GetComputeShader());

			RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, InitDirectionsTexture);
			RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, InitRadianceTexture);
			RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, InitHitDistanceTexture);
			DDGIMarkRadianceCascadeInitializationScheduled_RenderThread(Scene);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to create initialization UAVs."));
			return;
		}
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

	FShaderResourceViewRHIRef DirectionsSRV = RHICreateShaderResourceView(DirectionsTexture, 0);
	FShaderResourceViewRHIRef ProbePositionsSRV = RHICreateShaderResourceView(ProbePositionsTexture, 0);
	FUnorderedAccessViewRHIRef RadianceUAV = RHICreateUnorderedAccessView(RadianceTexture);
	FUnorderedAccessViewRHIRef HitDistanceUAV = RHICreateUnorderedAccessView(HitDistanceTexture);
	FUnorderedAccessViewRHIRef HitTriangleIndexUAV = RHICreateUnorderedAccessView(HitTriangleIndexTexture);
	if (!DirectionsSRV.IsValid() || !ProbePositionsSRV.IsValid() || !RadianceUAV.IsValid() || !HitDistanceUAV.IsValid() || !HitTriangleIndexUAV.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to create tracing SRV/UAV views."));
		return;
	}

	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, DirectionsTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, ProbePositionsTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, RadianceTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, HitDistanceTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, HitTriangleIndexTexture);

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
		RHICmdList, TraceShader.GetComputeShader(), DirectionsSRV, ProbePositionsSRV, NodesSRV, TrianglesSRV,
		RadianceUAV, HitDistanceUAV, HitTriangleIndexUAV,
		TotalTexels, GroupsX, ProbeCount, TotalRaysPerProbe,
		NodeCount, TriangleCount,
		static_cast<uint32>(Config->RadianceCascades.GetSafeCascadeCount()),
		Config->RadianceCascades.BaseSegmentLength, Config->RadianceCascades.CascadeScale,
		Config->RadianceCascades.MaxTraceDistance,
		FVector4(Config->StaticBackgroundColor.R, Config->StaticBackgroundColor.G, Config->StaticBackgroundColor.B, Config->StaticBackgroundColor.A),
		Config->StaticBackgroundIntensity, Config->GIIntensity,
		FVector4(Config->ProbeGridOrigin.X, Config->ProbeGridOrigin.Y, Config->ProbeGridOrigin.Z, 0.0f),
		Config->BackgroundTransitionStart, Config->BackgroundTransitionEnd,
		Config->bEnableStaticBackground ? 1u : 0u,
		FVector4(Config->DirectionalLightDirection.X, Config->DirectionalLightDirection.Y, Config->DirectionalLightDirection.Z, 0.0f),
		FVector4(Config->DirectionalLightColor.R, Config->DirectionalLightColor.G, Config->DirectionalLightColor.B, 0.0f),
		Config->DirectionalLightIntensity,
		Config->bHasDirectionalLight ? 1u : 0u);
	DispatchComputeShader(RHICmdList, TraceShader.GetShader(), GroupsX, GroupsY, 1);
	TraceShader->UnbindResources(RHICmdList, TraceShader.GetComputeShader());

	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, RadianceTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, HitDistanceTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, HitTriangleIndexTexture);

	FTexture2DRHIRef RadianceScratchTexture;
	FShaderResourceViewRHIRef SpatialHashSRV, NeighborListSRV;
	uint32 HashCapacity=0, SpatialProbeCount=0, NeighborListProbeCount=0;
	float ProbeSpacing=0.0f;
	const bool bHasNeighborList = DDGIGetRadianceCascadeProbeNeighborList_RenderThread(Scene,NeighborListSRV,NeighborListProbeCount) &&
		NeighborListSRV.IsValid() && NeighborListProbeCount==ProbeCount;
	const uint32 bUseNeighborList = (bHasNeighborList && CVarDDGIUsePrecomputedProbeNeighbors.GetValueOnRenderThread()!=0) ? 1u : 0u;
	if (!DDGIGetRadianceCascadeScratchTexture_RenderThread(Scene,RadianceScratchTexture) ||
		!DDGIGetRadianceCascadeProbePositionsRHI_RenderThread(Scene,ProbePositionsTexture,SpatialProbeCount) ||
		SpatialProbeCount!=ProbeCount) return;

	// The precomputed list is a complete neighbor lookup on its own. Do not
	// make that path depend on allocating the spatial-hash buffer as well.
	const bool bHasSpatialHash = DDGIGetRadianceCascadeProbeSpatialHash_RenderThread(
		Scene,SpatialHashSRV,HashCapacity,ProbeSpacing);
	if (bUseNeighborList)
	{
		if (!NeighborListSRV.IsValid()) return;
		ProbeSpacing = FMath::Max(1.0f,Config->ProbeSpacing);
	}
	else if (!bHasSpatialHash || !SpatialHashSRV.IsValid() || HashCapacity==0)
	{
		return;
	}

	FShaderResourceViewRHIRef GatherRadianceSRV=RHICreateShaderResourceView(RadianceTexture,0);
	FShaderResourceViewRHIRef GatherHitSRV=RHICreateShaderResourceView(HitDistanceTexture,0);
	FShaderResourceViewRHIRef GatherHitIndicesSRV=RHICreateShaderResourceView(HitTriangleIndexTexture,0);
	FShaderResourceViewRHIRef GatherDirectionsSRV=RHICreateShaderResourceView(DirectionsTexture,0);
	FShaderResourceViewRHIRef GatherPositionsSRV=RHICreateShaderResourceView(ProbePositionsTexture,0);
	FUnorderedAccessViewRHIRef GatherOutputUAV=RHICreateUnorderedAccessView(RadianceScratchTexture);
	if(!GatherRadianceSRV.IsValid()||!GatherHitSRV.IsValid()||!GatherHitIndicesSRV.IsValid()||!GatherDirectionsSRV.IsValid()||
		!GatherPositionsSRV.IsValid()||!TrianglesSRV.IsValid()||!GatherOutputUAV.IsValid()) return;
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable,ProbePositionsTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable,RadianceTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable,HitDistanceTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable,DirectionsTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable,RadianceScratchTexture);
	TShaderMapRef<FDDGIRadianceCascadeSpatialGatherRHI> GatherShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
	RHICmdList.SetComputeShader(GatherShader.GetComputeShader());
	GatherShader->SetParameters(RHICmdList,GatherShader.GetComputeShader(),GatherRadianceSRV,GatherHitSRV,GatherHitIndicesSRV,GatherDirectionsSRV,
		GatherPositionsSRV,TrianglesSRV,SpatialHashSRV,NeighborListSRV,GatherOutputUAV,TotalTexels,GroupsX,ProbeCount,
		TotalRaysPerProbe,HashCapacity,TriangleCount,ProbeSpacing,
		FVector4(Config->ProbeGridOrigin.X,Config->ProbeGridOrigin.Y,Config->ProbeGridOrigin.Z,0.0f),bUseNeighborList);
	DispatchComputeShader(RHICmdList,GatherShader.GetShader(),GroupsX,GroupsY,1);
	GatherShader->UnbindResources(RHICmdList,GatherShader.GetComputeShader());
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable,RadianceScratchTexture);

	// Cascade integration consumes the gathered radiance and ping-pongs separately.
	{
		FTexture2DRHIRef CurrentInputRadiance = RadianceScratchTexture;
		FTexture2DRHIRef CurrentOutputRadiance = RadianceTexture;
		const uint32 CascadeCount = static_cast<uint32>(Config->RadianceCascades.GetSafeCascadeCount());
		const uint32 TotalRays = static_cast<uint32>(Config->TotalRaysPerProbe);
		const uint32 TotalProbeTexels = ProbeCount * TotalRays;
		const uint32 IntegrationGroups = FMath::Min(FMath::DivideAndRoundUp(TotalProbeTexels, 64u), 65535u);
		const uint32 IntegrationGroupsY = FMath::DivideAndRoundUp(TotalProbeTexels, IntegrationGroups * 64u);
		bool bResultIsInScratch = true;

		for (int32 CascadeIndex = static_cast<int32>(CascadeCount) - 2; CascadeIndex >= 0; --CascadeIndex)
		{
			FShaderResourceViewRHIRef InputRadianceSRV = RHICreateShaderResourceView(CurrentInputRadiance, 0);
			FShaderResourceViewRHIRef HitDistanceSRV = RHICreateShaderResourceView(HitDistanceTexture, 0);
			FUnorderedAccessViewRHIRef OutputRadianceUAV = RHICreateUnorderedAccessView(CurrentOutputRadiance);
			if (!InputRadianceSRV.IsValid() || !HitDistanceSRV.IsValid() || !OutputRadianceUAV.IsValid())
			{
				UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to create cascade integration SRV/UAV views."));
				break;
			}

			RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, CurrentInputRadiance);
			RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, HitDistanceTexture);
			RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, CurrentOutputRadiance);

			TShaderMapRef<FDDGIRadianceCascadeIntegrateRHI> IntegrateShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
			RHICmdList.SetComputeShader(IntegrateShader.GetComputeShader());
			IntegrateShader->SetParameters(
				RHICmdList, IntegrateShader.GetComputeShader(), InputRadianceSRV, HitDistanceSRV, OutputRadianceUAV,
				TotalProbeTexels, IntegrationGroups, TotalRays,
				static_cast<uint32>(Config->RadianceCascades.GetRayStorageOffset(CascadeIndex)),
				static_cast<uint32>(Config->RadianceCascades.GetRayCount(CascadeIndex)),
				static_cast<uint32>(Config->RadianceCascades.GetRayStorageOffset(CascadeIndex + 1)),
				static_cast<uint32>(FMath::Max(1,
					Config->RadianceCascades.GetRayCount(CascadeIndex + 1) /
					FMath::Max(1, Config->RadianceCascades.GetRayCount(CascadeIndex)))));
			DispatchComputeShader(RHICmdList, IntegrateShader.GetShader(), IntegrationGroups, IntegrationGroupsY, 1);
			IntegrateShader->UnbindResources(RHICmdList, IntegrateShader.GetComputeShader());
			RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, CurrentOutputRadiance);

			Swap(CurrentInputRadiance, CurrentOutputRadiance);
			bResultIsInScratch = !bResultIsInScratch;
		}

		if (bResultIsInScratch)
		{
			DDGISwapRadianceCascadeRadianceBuffers_RenderThread(Scene);
		}
	}

}
