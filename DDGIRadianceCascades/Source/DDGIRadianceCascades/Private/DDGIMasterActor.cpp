#include "DDGIMasterActor.h"

ADDGIMasterActor::ADDGIMasterActor()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false;
}

#if WITH_EDITOR
void ADDGIMasterActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Keep the transition range valid when edited in the Details panel.
	if (BackgroundTransitionEnd < BackgroundTransitionStart)
	{
		BackgroundTransitionEnd = BackgroundTransitionStart;
	}
}
#endif
