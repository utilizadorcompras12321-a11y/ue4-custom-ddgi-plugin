#if WITH_DEV_AUTOMATION_TESTS

#include "DDGIRadianceCascadeConfig.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDDGIRadianceCascadeLayoutTest,
	"DDGI.RadianceCascades.Layout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDDGIRadianceCascadeLayoutTest::RunTest(const FString& Parameters)
{
	FDDGIRadianceCascadeConfig Config;
	Config.CascadeCount = 5;
	Config.BaseRayCount = 6;
	Config.RaySplitFactor = 2;
	Config.BaseSegmentLength = 250.0f;
	Config.CascadeScale = 2.0f;
	Config.MaxTraceDistance = 7750.0f;

	TestEqual(TEXT("Five configured cascades remain active"), Config.GetSafeCascadeCount(), 5);
	TestEqual(TEXT("Cascade 0 ray count"), Config.GetRayCount(0), 6);
	TestEqual(TEXT("Cascade 1 ray count"), Config.GetRayCount(1), 12);
	TestEqual(TEXT("Cascade 2 ray count"), Config.GetRayCount(2), 24);
	TestEqual(TEXT("Cascade 3 ray count"), Config.GetRayCount(3), 48);
	TestEqual(TEXT("Cascade 4 ray count"), Config.GetRayCount(4), 96);
	TestEqual(TEXT("Cascade 0 storage offset"), Config.GetRayStorageOffset(0), 0);
	TestEqual(TEXT("Cascade 1 storage offset"), Config.GetRayStorageOffset(1), 6);
	TestEqual(TEXT("Cascade 2 storage offset"), Config.GetRayStorageOffset(2), 18);
	TestEqual(TEXT("Cascade 3 storage offset"), Config.GetRayStorageOffset(3), 42);
	TestEqual(TEXT("Cascade 4 storage offset"), Config.GetRayStorageOffset(4), 90);
	TestEqual(TEXT("Total packed rays per probe"), Config.GetTotalRayCount(), 186);

	TestTrue(TEXT("Cascade intervals meet without gaps"),
		FMath::IsNearlyEqual(Config.GetCascadeEndDistance(0), Config.GetCascadeStartDistance(1)) &&
		FMath::IsNearlyEqual(Config.GetCascadeEndDistance(1), Config.GetCascadeStartDistance(2)) &&
		FMath::IsNearlyEqual(Config.GetCascadeEndDistance(2), Config.GetCascadeStartDistance(3)) &&
		FMath::IsNearlyEqual(Config.GetCascadeEndDistance(3), Config.GetCascadeStartDistance(4)));
	TestTrue(TEXT("Last cascade reaches maximum trace distance"),
		FMath::IsNearlyEqual(Config.GetCascadeEndDistance(4), Config.MaxTraceDistance));

	TArray<FDDGIRadianceCascadeSlice> Slices;
	Config.BuildCascadeSlices(Slices);
	TestEqual(TEXT("Packed slice count"), Slices.Num(), 5);
	if (Slices.Num() == 5)
	{
		TestEqual(TEXT("Packed final slice ray count"), Slices[4].RayCount, 96);
		TestEqual(TEXT("Packed final slice offset"), Slices[4].StorageOffset, 90);
	}

	const int64 ExpectedBytes = 10ll * (186ll * 32ll + 16ll) + 186ll * 8ll;
	TestEqual(TEXT("Storage estimate includes trace, gather, final and hit textures"),
		Config.GetEstimatedStorageBytes(10), ExpectedBytes);

	Config.MaxTraceDistance = 1000.0f;
	TestEqual(TEXT("Trailing cascades outside trace distance are excluded"),
		Config.GetSafeCascadeCount(), 3);
	TestEqual(TEXT("Packed ray count shrinks with active cascade count"),
		Config.GetTotalRayCount(), 42);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
