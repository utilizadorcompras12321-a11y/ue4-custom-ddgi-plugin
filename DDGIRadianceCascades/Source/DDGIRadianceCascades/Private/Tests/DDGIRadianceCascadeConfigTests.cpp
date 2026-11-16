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

	const int64 ExpectedBytes = 10ll * (186ll * 28ll + 16ll) + 186ll * 8ll;
	TestEqual(TEXT("Storage estimate includes raw, gather, packed final and hit textures"),
		Config.GetEstimatedStorageBytes(10), ExpectedBytes);

	Config.MaxTraceDistance = 1000.0f;
	TestEqual(TEXT("Trailing cascades outside trace distance are excluded"),
		Config.GetSafeCascadeCount(), 3);
	TestEqual(TEXT("Packed ray count shrinks with active cascade count"),
		Config.GetTotalRayCount(), 42);

	// Exercise the complete supported configuration range. The CPU layout
	// helpers are mirrored by RadianceCascadeLayout.ush and must never produce
	// overlapping storage ranges or gaps in the distance intervals.
	const float TraceDistances[] = { 1.0f, 249.0f, 250.0f, 1000.0f, 7750.0f, 1000000.0f };
	bool bAllCascadeCountsValid = true;
	bool bAllRayLayoutsBounded = true;
	bool bAllStorageOffsetsContiguous = true;
	bool bAllDistanceIntervalsValid = true;
	for (int32 CascadeCount = 1; CascadeCount <= 8; ++CascadeCount)
	{
		for (int32 BaseRays = 1; BaseRays <= 64; ++BaseRays)
		{
			for (int32 Split = 1; Split <= 4; ++Split)
			{
				for (float TraceDistance : TraceDistances)
				{
					FDDGIRadianceCascadeConfig Sweep;
					Sweep.CascadeCount = CascadeCount;
					Sweep.BaseRayCount = BaseRays;
					Sweep.RaySplitFactor = Split;
					Sweep.BaseSegmentLength = 250.0f;
					Sweep.CascadeScale = 2.0f;
					Sweep.MaxTraceDistance = TraceDistance;

					const int32 ActiveCount = Sweep.GetSafeCascadeCount();
					bAllCascadeCountsValid &= ActiveCount >= 1 && ActiveCount <= CascadeCount;
					bAllRayLayoutsBounded &= Sweep.GetTotalRayCount() >= BaseRays &&
						Sweep.GetTotalRayCount() <= 8 * 65536;

					int32 ExpectedOffset = 0;
					for (int32 Level = 0; Level < ActiveCount; ++Level)
					{
						bAllStorageOffsetsContiguous &= Sweep.GetRayStorageOffset(Level) == ExpectedOffset;
						ExpectedOffset += Sweep.GetRayCount(Level);
						bAllStorageOffsetsContiguous &= Sweep.GetRayStorageOffset(Level + 1) == ExpectedOffset;
						bAllDistanceIntervalsValid &= Sweep.GetCascadeEndDistance(Level) >
							Sweep.GetCascadeStartDistance(Level);
						if (Level + 1 < ActiveCount)
						{
							bAllDistanceIntervalsValid &= FMath::IsNearlyEqual(
								Sweep.GetCascadeEndDistance(Level),
								Sweep.GetCascadeStartDistance(Level + 1), 1.0e-3f);
						}
					}
					bAllDistanceIntervalsValid &= FMath::IsNearlyEqual(
						Sweep.GetCascadeEndDistance(ActiveCount - 1),
						TraceDistance, 1.0e-3f);
				}
			}
		}
	}

	TestTrue(TEXT("All supported cascade counts retain a valid active interval"), bAllCascadeCountsValid);
	TestTrue(TEXT("All supported ray layouts remain bounded"), bAllRayLayoutsBounded);
	TestTrue(TEXT("All supported ray storage offsets are contiguous"), bAllStorageOffsetsContiguous);
	TestTrue(TEXT("All supported distance intervals are contiguous and reach MaxTraceDistance"), bAllDistanceIntervalsValid);

	// The shader's 16-bit mantissa/exponent storage is independent of this
	// configuration calculation; verify that byte estimates remain monotonic.
	FDDGIRadianceCascadeConfig MemoryConfig;
	MemoryConfig.CascadeCount = 5;
	MemoryConfig.BaseRayCount = 6;
	MemoryConfig.RaySplitFactor = 2;
	const int64 OneProbeBytes = MemoryConfig.GetEstimatedStorageBytes(1);
	TestTrue(TEXT("Storage estimate grows with probe count"),
		MemoryConfig.GetEstimatedStorageBytes(2) > OneProbeBytes);
	TestTrue(TEXT("Storage estimate includes shared ray directions"),
		OneProbeBytes > MemoryConfig.GetTotalRayCount() * 28ll);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
