#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DDGIMasterActor.generated.h"

UCLASS(Blueprintable, ClassGroup = (Rendering), meta = (DisplayName = "Radiance Cascades Master Actor"))
class DDGIRADIANCECASCADES_API ADDGIMasterActor : public AActor
{
	GENERATED_BODY()

public:
	ADDGIMasterActor();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Global")
	bool bEnableDynamicGI = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Global", meta = (ClampMin = "0.0"))
	float GIIntensity = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Dynamic GI", meta = (ClampMin = "0.0", Units = "cm"))
	float DynamicGIDistance = 10000.0f;

	// Radiance Cascades layout. These settings define the cascade hierarchy,
	// not the scene tracing backend.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Radiance Cascades", meta = (ClampMin = "1", ClampMax = "8"))
	int32 CascadeCount = 5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Radiance Cascades", meta = (ClampMin = "1", ClampMax = "64"))
	int32 BaseRayCount = 6;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Radiance Cascades", meta = (ClampMin = "1", ClampMax = "4"))
	int32 RaySplitFactor = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Radiance Cascades", meta = (ClampMin = "1.0", Units = "cm"))
	float BaseSegmentLength = 250.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Radiance Cascades", meta = (ClampMin = "1.01"))
	float CascadeScale = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Dynamic GI", meta = (ClampMin = "1.0", Units = "cm"))
	float ProbeSpacing = 200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Performance", meta = (ClampMin = "1"))
	int32 MaxActiveProbes = 32768;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DDGI|Performance", meta = (ClampMin = "0"))
	int32 ProbeUpdateBudget = 50000;

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
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	void RefreshWorldConfig();
};
