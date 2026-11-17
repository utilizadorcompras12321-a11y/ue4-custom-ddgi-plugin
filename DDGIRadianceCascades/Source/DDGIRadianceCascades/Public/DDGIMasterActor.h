#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

class USceneComponent;
#include "DDGIMasterActor.generated.h"

UCLASS(Blueprintable, ClassGroup = (Rendering), meta = (DisplayName = "Radiance Cascades Master Actor"))
class DDGIRADIANCECASCADES_API ADDGIMasterActor : public AActor
{
	GENERATED_BODY()

public:
	ADDGIMasterActor();

	// Invisible root used to expose a reliable, editable world transform in the editor.
	UPROPERTY(VisibleDefaultsOnly, Category = "DDGI|Global")
	USceneComponent* SceneRootComponent;

	// Rebuild the static-scene acceleration structure after loading or changing static geometry.
	// This is an explicit, potentially expensive operation; it is not performed every frame.
	UFUNCTION(BlueprintCallable, Category = "DDGI|Radiance Cascades")
	void RebuildStaticGeometryBVH();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Global")
	bool bEnableDynamicGI = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Global", meta = (ClampMin = "0.0"))
	float GIIntensity = 1.0f;

	// Optional artistic multiplier applied after reading the directional light in lux. 1.0 preserves its physical intensity.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Lighting", meta = (ClampMin = "0.0", ClampMax = "100.0", UIMin = "0.0", UIMax = "2.0"))
	float DirectionalLightGIIntensityScale = 1.0f;

	// Scale for local point/spot-light irradiance captured by the software tracer.
	// Inverse-square lights are converted from lumens to approximate candela.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Lighting", meta = (ClampMin = "0.0", ClampMax = "100.0", UIMin = "0.0", UIMax = "2.0"))
	float LocalLightGIIntensityScale = 1.0f;

	// Maximum distance traced by each radiance ray. Independent from the local probe-grid coverage radius.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Dynamic GI", meta = (ClampMin = "1.0", Units = "cm"))
	float DynamicGIDistance = 10000.0f;

	// Radius covered by the probe lattice. A smaller local radius preserves probe density under the GPU probe budget.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Dynamic GI", meta = (ClampMin = "1.0", ClampMax = "12000000.0", Units = "cm"))
	float ProbeGridDistance = 4000.0f;

	// Radiance Cascades layout. These settings define the cascade hierarchy,
	// not the scene tracing backend.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Radiance Cascades", meta = (ClampMin = "1", ClampMax = "8"))
	int32 CascadeCount = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Radiance Cascades", meta = (ClampMin = "1", ClampMax = "64"))
	int32 BaseRayCount = 32;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Radiance Cascades", meta = (ClampMin = "1.0", Units = "cm"))
	float BaseSegmentLength = 250.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Radiance Cascades", meta = (ClampMin = "1.01"))
	float CascadeScale = 2.0f;

	// Base spacing for spatial probe level 0. Higher clipmap levels use power-of-two steps {1, 2, 4, 8}.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Dynamic GI", meta = (ClampMin = "1.0", ClampMax = "100000.0", Units = "cm"))
	float ProbeSpacing = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Performance", meta = (ClampMin = "1", ClampMax = "32768", UIMin = "1", UIMax = "32768"))
	int32 MaxActiveProbes = 32768;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Performance", meta = (ClampMin = "1", ClampMax = "250000", UIMin = "1000", UIMax = "250000"))
	int32 MaxSoftwareTriangles = 250000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Background")
	bool bEnableStaticBackground = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Background")
	FLinearColor StaticBackgroundColor = FLinearColor(0.025f, 0.035f, 0.055f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Background", meta = (ClampMin = "0.0"))
	float StaticBackgroundIntensity = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Transition", meta = (ClampMin = "0.0", Units = "cm"))
	float BackgroundTransitionStart = 8000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Transition", meta = (ClampMin = "0.0", Units = "cm"))
	float BackgroundTransitionEnd = 10000.0f;

protected:
	virtual void BeginPlay() override;
	virtual void PostRegisterAllComponents() override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	void RefreshWorldConfig();
};
