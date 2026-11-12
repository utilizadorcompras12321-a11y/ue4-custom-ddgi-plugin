#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DDGIMasterActor.generated.h"

/**
 * Level-wide configuration actor for DDGI Radiance Cascades.
 *
 * Place one instance in each level. This actor owns configuration;
 * rendering and probe updates are handled by the plugin runtime.
 */
UCLASS(Blueprintable, ClassGroup = (Rendering), meta = (DisplayName = "DDGI Master Actor"))
class DDGIRADIANCECASCADES_API ADDGIMasterActor : public AActor
{
	GENERATED_BODY()

public:
	ADDGIMasterActor();

	/** Enables the dynamic GI system for this level. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Global")
	bool bEnableDynamicGI = true;

	/** Multiplier applied to the dynamic indirect lighting contribution. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Global", meta = (ClampMin = "0.0"))
	float GIIntensity = 1.0f;

	/** Maximum distance from the active view origin that uses dynamic GI, in Unreal units. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Dynamic GI", meta = (ClampMin = "0.0", Units = "cm"))
	float DynamicGIDistance = 10000.0f;

	/** Desired world-space spacing between sparse probes, in Unreal units. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Dynamic GI", meta = (ClampMin = "1.0", Units = "cm"))
	float ProbeSpacing = 200.0f;

	/** Upper bound for the number of active probes managed by the level. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Performance", meta = (ClampMin = "1"))
	int32 MaxActiveProbes = 32768;

	/** Probe update work budget per frame. A value of zero means unlimited. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Performance", meta = (ClampMin = "0"))
	int32 ProbeUpdateBudget = 50000;

	/** Enables the static diffuse contribution used beyond the dynamic GI range. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Background")
	bool bEnableStaticBackground = true;

	/** Diffuse background color in linear space. This does not replace direct lighting. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Background")
	FLinearColor StaticBackgroundColor = FLinearColor(0.025f, 0.035f, 0.055f, 1.0f);

	/** Intensity multiplier for the static background contribution. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Background", meta = (ClampMin = "0.0"))
	float StaticBackgroundIntensity = 1.0f;

	/** Start of the blend from dynamic GI to static background, measured from the view origin. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Transition", meta = (ClampMin = "0.0", Units = "cm"))
	float BackgroundTransitionStart = 8000.0f;

	/** End of the blend; at and beyond this distance the static background is used. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Transition", meta = (ClampMin = "0.0", Units = "cm"))
	float BackgroundTransitionEnd = 10000.0f;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
