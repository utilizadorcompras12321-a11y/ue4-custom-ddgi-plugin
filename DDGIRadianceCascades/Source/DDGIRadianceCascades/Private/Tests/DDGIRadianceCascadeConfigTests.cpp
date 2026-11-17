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
	Config.BaseSegmentLength = 250.0f;
	Config.CascadeScale = 2.0f;
	Config.MaxTraceDistance = 7750.0f;

	TestEqual(TEXT("Five configured cascades remain active"), Config.GetSafeCascadeCount(), 5);
	for (int32 Cascade = 0; Cascade < 5; ++Cascade)
	{
		TestEqual(TEXT("Every cascade uses the same ray count"), Config.GetRayCount(Cascade), 6);
		TestEqual(TEXT("Cascade storage offset is contiguous"), Config.GetRayStorageOffset(Cascade), Cascade * 6);
	}
	TestEqual(TEXT("Total rays per probe without splitting"), Config.GetTotalRayCount(), 30);

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
		for (int32 Cascade = 0; Cascade < 5; ++Cascade)
		{
			TestEqual(TEXT("Slice ray count remains fixed"), Slices[Cascade].RayCount, 6);
			TestEqual(TEXT("Slice offset remains contiguous"), Slices[Cascade].StorageOffset, Cascade * 6);
		}
	}

	const int64 ExpectedBytes = 10ll * (30ll * 28ll + 16ll) + 30ll * 8ll;
	TestEqual(TEXT("Storage estimate reflects fixed ray count"),
		Config.GetEstimatedStorageBytes(10), ExpectedBytes);

	Config.MaxTraceDistance = 1000.0f;
	TestEqual(TEXT("Trailing cascades outside trace distance are excluded"), Config.GetSafeCascadeCount(), 3);
	TestEqual(TEXT("Packed ray count shrinks with active cascade count"), Config.GetTotalRayCount(), 18);

	const float TraceDistances[] = { 1.0f, 249.0f, 250.0f, 1000.0f, 7750.0f, 1000000.0f };
	bool bAllCascadeCountsValid = true;
	bool bAllRayCountsFixed = true;
	bool bAllStorageOffsetsContiguous = true;
	bool bAllDistanceIntervalsValid = true;
	for (int32 CascadeCount = 1; CascadeCount <= 8; ++CascadeCount)
	{
		for (int32 BaseRays = 1; BaseRays <= 64; ++BaseRays)
		{
			for (float TraceDistance : TraceDistances)
			{
				FDDGIRadianceCascadeConfig Sweep;
				Sweep.CascadeCount = CascadeCount;
				Sweep.BaseRayCount = BaseRays;
				Sweep.BaseSegmentLength = 250.0f;
				Sweep.CascadeScale = 2.0f;
				Sweep.MaxTraceDistance = TraceDistance;

				const int32 ActiveCount = Sweep.GetSafeCascadeCount();
				bAllCascadeCountsValid &= ActiveCount >= 1 && ActiveCount <= CascadeCount;
				bAllRayCountsFixed &= Sweep.GetTotalRayCount() == ActiveCount * BaseRays;

				int32 ExpectedOffset = 0;
				for (int32 Level = 0; Level < ActiveCount; ++Level)
				{
					bAllStorageOffsetsContiguous &= Sweep.GetRayStorageOffset(Level) == ExpectedOffset;
					ExpectedOffset += BaseRays;
					bAllStorageOffsetsContiguous &= Sweep.GetRayStorageOffset(Level + 1) == ExpectedOffset;
					bAllDistanceIntervalsValid &= Sweep.GetCascadeEndDistance(Level) > Sweep.GetCascadeStartDistance(Level);
					if (Level + 1 < ActiveCount)
					{
						bAllDistanceIntervalsValid &= FMath::IsNearlyEqual(
							Sweep.GetCascadeEndDistance(Level), Sweep.GetCascadeStartDistance(Level + 1), 1.0e-3f);
					}
				}
				bAllDistanceIntervalsValid &= FMath::IsNearlyEqual(
					Sweep.GetCascadeEndDistance(ActiveCount - 1), TraceDistance, 1.0e-3f);
			}
		}
	}

	TestTrue(TEXT("All supported cascade counts retain a valid active interval"), bAllCascadeCountsValid);
	TestTrue(TEXT("Every cascade has a fixed, non-growing ray count"), bAllRayCountsFixed);
	TestTrue(TEXT("All ray storage offsets are contiguous"), bAllStorageOffsetsContiguous);
	TestTrue(TEXT("All distance intervals are contiguous and reach MaxTraceDistance"), bAllDistanceIntervalsValid);

	const int64 OneProbeBytes = Config.GetEstimatedStorageBytes(1);
	TestTrue(TEXT("Storage estimate grows with probe count"), Config.GetEstimatedStorageBytes(2) > OneProbeBytes);
	TestTrue(TEXT("Storage estimate includes shared ray directions"), OneProbeBytes > Config.GetTotalRayCount() * 28ll);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
