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
		HitTriangleIndexUAV.Bind(Initializer.ParameterMap, TEXT("HitTriangleIndexUAV"));
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
		Ar << HitTriangleIndexUAV;
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
	FShaderResourceParameter HitTriangleIndexUAV;
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
		Ar << CascadeCount;
		Ar << BaseSegmentLength;
		Ar << CascadeScale;
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
		FComputeShaderRHIParamRef ShaderRHI = GetComputeShader();
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
		if (RayOriginsTexture.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, RayOriginsTexture.GetBaseIndex(), OriginsSRV);
		if (BVHNodes.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, BVHNodes.GetBaseIndex(), NodesSRV);
		if (BVHTriangles.IsBound()) RHICmdList.SetShaderResourceViewParameter(ShaderRHI, BVHTriangles.GetBaseIndex(), TrianglesSRV);
		if (RadianceUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, RadianceUAV.GetBaseIndex(), RadianceView);
		if (HitDistanceUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, HitDistanceUAV.GetBaseIndex(), HitDistanceView);
		if (HitTriangleIndexUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, HitTriangleIndexUAV.GetBaseIndex(), HitTriangleIndexView);
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
		if (HitTriangleIndexUAV.IsBound()) RHICmdList.SetUAVParameter(ShaderRHI, HitTriangleIndexUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
	}

private:
	FShaderParameter TextureWidth;
	FShaderParameter TotalTexels;
	FShaderParameter DispatchGroupsX;
	FShaderParameter ProbeCount;
	FShaderParameter TotalRaysPerProbe;
	FShaderParameter NodeCount;
	FShaderParameter TriangleCount;
	FShaderParameter CascadeCount;
	FShaderParameter BaseSegmentLength;
	FShaderParameter CascadeScale;
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
  ProbeSpacing.Bind(I.ParameterMap,TEXT("ProbeSpacing")); ProbeGridOrigin.Bind(I.ParameterMap,TEXT("ProbeGridOrigin"));
  RadianceTexture.Bind(I.ParameterMap,TEXT("RadianceTexture")); HitDistanceTexture.Bind(I.ParameterMap,TEXT("HitDistanceTexture")); HitTriangleIndexTexture.Bind(I.ParameterMap,TEXT("HitTriangleIndexTexture"));
  DirectionsTexture.Bind(I.ParameterMap,TEXT("DirectionsTexture")); RayOriginsTexture.Bind(I.ParameterMap,TEXT("RayOriginsTexture"));
  ProbePositionsTexture.Bind(I.ParameterMap,TEXT("ProbePositionsTexture")); BVHTriangles.Bind(I.ParameterMap,TEXT("BVHTriangles"));
  ProbeSpatialHash.Bind(I.ParameterMap,TEXT("ProbeSpatialHash")); OutputRadianceUAV.Bind(I.ParameterMap,TEXT("OutputRadianceUAV"));
 }
 static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P){return IsFeatureLevelSupported(P.Platform,ERHIFeatureLevel::SM5);}
 virtual bool Serialize(FArchive& A) override
 {
  bool O=FGlobalShader::Serialize(A); A<<TextureWidth<<TotalTexels<<DispatchGroupsX<<ProbeCount<<TotalRaysPerProbe;
  A<<HashCapacity<<ProbeSpacing<<ProbeGridOrigin<<RadianceTexture<<HitDistanceTexture<<HitTriangleIndexTexture<<DirectionsTexture<<RayOriginsTexture;
  A<<ProbePositionsTexture<<BVHTriangles<<ProbeSpatialHash<<OutputRadianceUAV; return O;
 }
 void SetParameters(FRHICommandList& C,FShaderResourceViewRHIRef R,FShaderResourceViewRHIRef H,FShaderResourceViewRHIRef HitIndices,
  FShaderResourceViewRHIRef D,FShaderResourceViewRHIRef Origins,FShaderResourceViewRHIRef P,FShaderResourceViewRHIRef Triangles,FShaderResourceViewRHIRef S,FUnorderedAccessViewRHIRef O,
  uint32 T,uint32 G,uint32 PC,uint32 RPP,uint32 HC,float PS,const FVector4& Origin)
 {
  auto Sh=GetComputeShader(); uint32 W=4096u;
  SetShaderValue(C,Sh,TextureWidth,W); SetShaderValue(C,Sh,TotalTexels,T); SetShaderValue(C,Sh,DispatchGroupsX,G);
  SetShaderValue(C,Sh,ProbeCount,PC); SetShaderValue(C,Sh,TotalRaysPerProbe,RPP); SetShaderValue(C,Sh,HashCapacity,HC);
  SetShaderValue(C,Sh,ProbeSpacing,PS); SetShaderValue(C,Sh,ProbeGridOrigin,Origin);
  if(RadianceTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,RadianceTexture.GetBaseIndex(),R);
  if(HitDistanceTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,HitDistanceTexture.GetBaseIndex(),H);
  if(HitTriangleIndexTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,HitTriangleIndexTexture.GetBaseIndex(),HitIndices);
  if(DirectionsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,DirectionsTexture.GetBaseIndex(),D);
  if(RayOriginsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,RayOriginsTexture.GetBaseIndex(),Origins);
  if(ProbePositionsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,ProbePositionsTexture.GetBaseIndex(),P);
  if(BVHTriangles.IsBound()) C.SetShaderResourceViewParameter(Sh,BVHTriangles.GetBaseIndex(),Triangles);
  if(ProbeSpatialHash.IsBound()) C.SetShaderResourceViewParameter(Sh,ProbeSpatialHash.GetBaseIndex(),S);
  if(OutputRadianceUAV.IsBound()) C.SetUAVParameter(Sh,OutputRadianceUAV.GetBaseIndex(),O);
 }
 void UnbindResources(FRHICommandList& C)
 {
  auto Sh=GetComputeShader();
  if(RadianceTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,RadianceTexture.GetBaseIndex(),FShaderResourceViewRHIParamRef());
  if(HitDistanceTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,HitDistanceTexture.GetBaseIndex(),FShaderResourceViewRHIParamRef());
  if(HitTriangleIndexTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,HitTriangleIndexTexture.GetBaseIndex(),FShaderResourceViewRHIParamRef());
  if(DirectionsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,DirectionsTexture.GetBaseIndex(),FShaderResourceViewRHIParamRef());
  if(RayOriginsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,RayOriginsTexture.GetBaseIndex(),FShaderResourceViewRHIParamRef());
  if(ProbePositionsTexture.IsBound()) C.SetShaderResourceViewParameter(Sh,ProbePositionsTexture.GetBaseIndex(),FShaderResourceViewRHIParamRef());
  if(BVHTriangles.IsBound()) C.SetShaderResourceViewParameter(Sh,BVHTriangles.GetBaseIndex(),FShaderResourceViewRHIParamRef());
  if(ProbeSpatialHash.IsBound()) C.SetShaderResourceViewParameter(Sh,ProbeSpatialHash.GetBaseIndex(),FShaderResourceViewRHIParamRef());
  if(OutputRadianceUAV.IsBound()) C.SetUAVParameter(Sh,OutputRadianceUAV.GetBaseIndex(),FUnorderedAccessViewRHIRef());
 }
private:
 FShaderParameter TextureWidth,TotalTexels,DispatchGroupsX,ProbeCount,TotalRaysPerProbe,HashCapacity,ProbeSpacing,ProbeGridOrigin;
 FShaderResourceParameter RadianceTexture,HitDistanceTexture,HitTriangleIndexTexture,DirectionsTexture,RayOriginsTexture,ProbePositionsTexture,BVHTriangles,ProbeSpatialHash,OutputRadianceUAV;
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

	virtual bool Serialize(FArchive& Ar) override
	{
		const bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		Ar << TextureWidth;
		Ar << TotalTexels;
		Ar << DispatchGroupsX;
		Ar << TotalRaysPerProbe;
		Ar << CascadeStorageOffset;
		Ar << CascadeRayCount;
		Ar << NextCascadeStorageOffset;
		Ar << EffectiveSplitFactor;
		Ar << InputRadianceTexture;
		Ar << HitDistanceTexture;
		Ar << OutputRadianceUAV;
		return bShaderHasOutdatedParameters;
	}

	void SetParameters(
		FRHICommandList& RHICmdList,
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
		FComputeShaderRHIParamRef ShaderRHI = GetComputeShader();
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

	void UnbindResources(FRHICommandList& RHICmdList)
	{
		FComputeShaderRHIParamRef ShaderRHI = GetComputeShader();
		if (InputRadianceTexture.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, InputRadianceTexture.GetBaseIndex(), FShaderResourceViewRHIParamRef());
		}
		if (HitDistanceTexture.IsBound())
		{
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI, HitDistanceTexture.GetBaseIndex(), FShaderResourceViewRHIParamRef());
		}
		if (OutputRadianceUAV.IsBound())
		{
			RHICmdList.SetUAVParameter(ShaderRHI, OutputRadianceUAV.GetBaseIndex(), FUnorderedAccessViewRHIRef());
		}
	}

private:
	FShaderParameter TextureWidth;
	FShaderParameter TotalTexels;
	FShaderParameter DispatchGroupsX;
	FShaderParameter TotalRaysPerProbe;
	FShaderParameter CascadeStorageOffset;
	FShaderParameter CascadeRayCount;
	FShaderParameter NextCascadeStorageOffset;
	FShaderParameter EffectiveSplitFactor;
	FShaderResourceParameter InputRadianceTexture;
	FShaderResourceParameter HitDistanceTexture;
	FShaderResourceParameter OutputRadianceUAV;
};

IMPLEMENT_SHADER_TYPE(
	,
	FDDGIRadianceCascadeIntegrateRHI,
	TEXT("/Plugin/DDGIRadianceCascades/Private/RadianceCascades/RadianceCascadeIntegrateCS.usf"),
	TEXT("RadianceCascadeIntegrateCS"),
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
	FTexture2DRHIRef HitTriangleIndexTexture;
	FTexture2DRHIRef RayOriginsTexture;
	uint32 ProbeCount = 0;
	uint32 TotalRaysPerProbe = 0;
	if (!DDGIGetRadianceCascadeTracingRHIResources_RenderThread(
		Scene, DirectionsTexture, RadianceTexture, HitDistanceTexture, HitTriangleIndexTexture, RayOriginsTexture,
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
		// The resource manager supplies valid zero-count buffers for an empty BVH.
		return;
	}

	FShaderResourceViewRHIRef DirectionsSRV = RHICreateShaderResourceView(DirectionsTexture, 0);
	FShaderResourceViewRHIRef OriginsSRV = RHICreateShaderResourceView(RayOriginsTexture, 0);
	FUnorderedAccessViewRHIRef RadianceUAV = RHICreateUnorderedAccessView(RadianceTexture);
	FUnorderedAccessViewRHIRef HitDistanceUAV = RHICreateUnorderedAccessView(HitDistanceTexture);
	FUnorderedAccessViewRHIRef HitTriangleIndexUAV = RHICreateUnorderedAccessView(HitTriangleIndexTexture);
	if (!DirectionsSRV.IsValid() || !OriginsSRV.IsValid() || !RadianceUAV.IsValid() || !HitDistanceUAV.IsValid() || !HitTriangleIndexUAV.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Radiance Cascades: failed to create tracing SRV/UAV views."));
		return;
	}

	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, DirectionsTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, RayOriginsTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, RadianceTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, HitDistanceTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, HitTriangleIndexTexture);

	TShaderMapRef<FDDGIRadianceCascadeTraceRHI> TraceShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
	RHICmdList.SetComputeShader(TraceShader->GetComputeShader());
	TraceShader->SetParameters(
		RHICmdList, DirectionsSRV, OriginsSRV, NodesSRV, TrianglesSRV,
		RadianceUAV, HitDistanceUAV, HitTriangleIndexUAV,
		TotalTexels, GroupsX, ProbeCount, TotalRaysPerProbe,
		NodeCount, TriangleCount,
		static_cast<uint32>(Config.RadianceCascades.GetSafeCascadeCount()),
		Config.RadianceCascades.BaseSegmentLength, Config.RadianceCascades.CascadeScale,
		Config.RadianceCascades.MaxTraceDistance,
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
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, HitTriangleIndexTexture);

	FTexture2DRHIRef RadianceScratchTexture, ProbePositionsTexture;
	FShaderResourceViewRHIRef SpatialHashSRV;
	uint32 HashCapacity=0, SpatialProbeCount=0;
	float ProbeSpacing=0.0f;
	if (!DDGIGetRadianceCascadeScratchTexture_RenderThread(Scene,RadianceScratchTexture) ||
		!DDGIGetRadianceCascadeProbePositionsRHI_RenderThread(Scene,ProbePositionsTexture,SpatialProbeCount) ||
		!DDGIGetRadianceCascadeProbeSpatialHash_RenderThread(Scene,SpatialHashSRV,HashCapacity,ProbeSpacing) ||
		SpatialProbeCount!=ProbeCount || !SpatialHashSRV.IsValid() || HashCapacity==0) return;

	FShaderResourceViewRHIRef GatherRadianceSRV=RHICreateShaderResourceView(RadianceTexture,0);
	FShaderResourceViewRHIRef GatherHitSRV=RHICreateShaderResourceView(HitDistanceTexture,0);
	FShaderResourceViewRHIRef GatherHitIndicesSRV=RHICreateShaderResourceView(HitTriangleIndexTexture,0);
	FShaderResourceViewRHIRef GatherDirectionsSRV=RHICreateShaderResourceView(DirectionsTexture,0);
	FShaderResourceViewRHIRef GatherOriginsSRV=RHICreateShaderResourceView(RayOriginsTexture,0);
	FShaderResourceViewRHIRef GatherPositionsSRV=RHICreateShaderResourceView(ProbePositionsTexture,0);
	FUnorderedAccessViewRHIRef GatherOutputUAV=RHICreateUnorderedAccessView(RadianceScratchTexture);
	if(!GatherRadianceSRV.IsValid()||!GatherHitSRV.IsValid()||!GatherHitIndicesSRV.IsValid()||!GatherDirectionsSRV.IsValid()||
		!GatherOriginsSRV.IsValid()||!GatherPositionsSRV.IsValid()||!TrianglesSRV.IsValid()||!GatherOutputUAV.IsValid()) return;
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable,ProbePositionsTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable,RadianceTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable,HitDistanceTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable,DirectionsTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable,RayOriginsTexture);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable,RadianceScratchTexture);
	TShaderMapRef<FDDGIRadianceCascadeSpatialGatherRHI> GatherShader(GetGlobalShaderMap(ERHIFeatureLevel::SM5));
	RHICmdList.SetComputeShader(GatherShader->GetComputeShader());
	GatherShader->SetParameters(RHICmdList,GatherRadianceSRV,GatherHitSRV,GatherHitIndicesSRV,GatherDirectionsSRV,GatherOriginsSRV,
		GatherPositionsSRV,TrianglesSRV,SpatialHashSRV,GatherOutputUAV,TotalTexels,GroupsX,ProbeCount,
		TotalRaysPerProbe,HashCapacity,ProbeSpacing,
		FVector4(Config.ProbeGridOrigin.X,Config.ProbeGridOrigin.Y,Config.ProbeGridOrigin.Z,0.0f));
	DispatchComputeShader(RHICmdList,*GatherShader,GroupsX,GroupsY,1);
	GatherShader->UnbindResources(RHICmdList);
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable,RadianceScratchTexture);

	// Cascade integration consumes the gathered radiance and ping-pongs separately.
	{
		FTexture2DRHIRef CurrentInputRadiance = RadianceScratchTexture;
		FTexture2DRHIRef CurrentOutputRadiance = RadianceTexture;
		const uint32 CascadeCount = static_cast<uint32>(Config.RadianceCascades.GetSafeCascadeCount());
		const uint32 TotalRays = static_cast<uint32>(Config.TotalRaysPerProbe);
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
			RHICmdList.SetComputeShader(IntegrateShader->GetComputeShader());
			IntegrateShader->SetParameters(
				RHICmdList, InputRadianceSRV, HitDistanceSRV, OutputRadianceUAV,
				TotalProbeTexels, IntegrationGroups, TotalRays,
				static_cast<uint32>(Config.RadianceCascades.GetRayStorageOffset(CascadeIndex)),
				static_cast<uint32>(Config.RadianceCascades.GetRayCount(CascadeIndex)),
				static_cast<uint32>(Config.RadianceCascades.GetRayStorageOffset(CascadeIndex + 1)),
				static_cast<uint32>(FMath::Max(1,
					Config.RadianceCascades.GetRayCount(CascadeIndex + 1) /
					FMath::Max(1, Config.RadianceCascades.GetRayCount(CascadeIndex)))));
			DispatchComputeShader(RHICmdList, *IntegrateShader, IntegrationGroups, IntegrationGroupsY, 1);
			IntegrateShader->UnbindResources(RHICmdList);
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
