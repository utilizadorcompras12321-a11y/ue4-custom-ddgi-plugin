#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "DDGIRadianceCascadeConfig.h"
#include "DDGIMasterSubsystem.generated.h"

class ADDGIMasterActor;
class UDirectionalLightComponent;
class FSceneInterface;

struct FDDGISoftwareBVHData
{
	TArray<FVector4> PackedNodes;
	TArray<FVector4> PackedTriangles;
	int32 IncludedComponents = 0;
	int32 SkippedNoCPUAccess = 0;
	int32 SkippedMovable = 0;
	int32 SkippedInstanced = 0;
	int32 SkippedInvalidTriangles = 0;
	int32 MaterialsUsingDefaultBaseColor = 0;
	int32 TriangleCount = 0;
	int32 NodeCount = 0;
};

USTRUCT()
struct FDDGIMasterConfig
{
	GENERATED_BODY()

	UPROPERTY() bool bEnableDynamicGI = true;
	UPROPERTY() float GIIntensity = 1.0f;
	UPROPERTY() bool bHasDirectionalLight = false;
	UPROPERTY() FVector DirectionalLightDirection = FVector(0.0f, 0.0f, -1.0f);
	UPROPERTY() FLinearColor DirectionalLightColor = FLinearColor::White;
	UPROPERTY() float DirectionalLightIntensity = 0.0f;
	UPROPERTY() float DynamicGIDistance = 10000.0f;
	UPROPERTY() FDDGIRadianceCascadeConfig RadianceCascades;
	UPROPERTY() TArray<FDDGIRadianceCascadeSlice> RadianceCascadeSlices;
	UPROPERTY() int32 TotalRaysPerProbe = 0;
	UPROPERTY() int64 EstimatedRadianceStorageBytes = 0;
	UPROPERTY() float ProbeSpacing = 200.0f;
	UPROPERTY() FVector ProbeGridOrigin = FVector::ZeroVector;
	UPROPERTY() TArray<FVector4> ProbePositions;
	UPROPERTY() int32 ProbeCount = 0;
	UPROPERTY() int32 MaxActiveProbes = 32768;
	UPROPERTY() int32 ProbeUpdateBudget = 50000;
	TSharedPtr<FDDGISoftwareBVHData, ESPMode::ThreadSafe> SoftwareBVH;
	int32 MaxSoftwareTriangles = 250000;
	UPROPERTY() bool bEnableStaticBackground = true;
	UPROPERTY() FLinearColor StaticBackgroundColor = FLinearColor(0.025f, 0.035f, 0.055f, 1.0f);
	UPROPERTY() float StaticBackgroundIntensity = 1.0f;
	UPROPERTY() float BackgroundTransitionStart = 8000.0f;
	UPROPERTY() float BackgroundTransitionEnd = 10000.0f;
};

DDGIRADIANCECASCADES_API bool DDGIGetMasterConfig_RenderThread(
	const FSceneInterface* Scene,
	FDDGIMasterConfig& OutConfig);

UCLASS()
class DDGIRADIANCECASCADES_API UDDGIMasterSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	void RegisterMasterActor(ADDGIMasterActor* InActor);
	void UnregisterMasterActor(ADDGIMasterActor* InActor);
	void RefreshConfig(ADDGIMasterActor* InActor);
	void RefreshDirectionalLighting(ADDGIMasterActor* InActor);

	const FDDGIMasterConfig& GetConfig() const { return Config; }
	ADDGIMasterActor* GetMasterActor() const { return ActiveMasterActor.Get(); }
	bool HasMasterActor() const { return ActiveMasterActor.IsValid(); }

private:
	int32 CachedSoftwareBVHTriangleLimit = INDEX_NONE;
	TWeakObjectPtr<UDirectionalLightComponent> ActiveDirectionalLight;
	bool bHasSearchedDirectionalLights = false;
	bool UpdateDirectionalLighting(const ADDGIMasterActor* InActor);
	void PublishConfigValuesOnly();
	void CopyConfigFromActor(const ADDGIMasterActor* InActor);
	void PublishConfig(bool bHasConfig);

	UPROPERTY(Transient)
	TWeakObjectPtr<ADDGIMasterActor> ActiveMasterActor;

	UPROPERTY(Transient)
	FDDGIMasterConfig Config;
};
