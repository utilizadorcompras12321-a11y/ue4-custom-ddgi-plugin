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
	Config.CascadeCount = 3;
	Config.BaseRayCount = 16;
	Config.BaseSegmentLength = 250.0f;
	Config.CascadeScale = 2.0f;
	Config.MaxTraceDistance = 7750.0f;

	TestEqual(TEXT("Three cascades fit the 128-ray budget"), Config.GetSafeCascadeCount(), 3);
	TestEqual(TEXT("Cascade zero ray count"), Config.GetRayCount(0), 16);
	TestEqual(TEXT("Cascade one doubles its ray count"), Config.GetRayCount(1), 32);
	TestEqual(TEXT("Cascade two doubles its ray count"), Config.GetRayCount(2), 64);
	TestEqual(TEXT("Cascade storage offsets are cumulative"), Config.GetRayStorageOffset(0), 0);
	TestEqual(TEXT("Cascade one begins after cascade zero"), Config.GetRayStorageOffset(1), 16);
	TestEqual(TEXT("Cascade two begins after cascades zero and one"), Config.GetRayStorageOffset(2), 48);
	TestEqual(TEXT("Total rays per probe"), Config.GetTotalRayCount(), 112);

	TestTrue(TEXT("Cascade intervals meet without gaps"),
		FMath::IsNearlyEqual(Config.GetCascadeEndDistance(0), Config.GetCascadeStartDistance(1)) &&
		FMath::IsNearlyEqual(Config.GetCascadeEndDistance(1), Config.GetCascadeStartDistance(2)));
	TestTrue(TEXT("Last active cascade reaches maximum trace distance"),
		FMath::IsNearlyEqual(Config.GetCascadeEndDistance(2), Config.MaxTraceDistance));

	TArray<FDDGIRadianceCascadeSlice> Slices;
	Config.BuildCascadeSlices(Slices);
	TestEqual(TEXT("Packed slice count"), Slices.Num(), 3);
	if (Slices.Num() == 3)
	{
		const int32 ExpectedRays[] = { 16, 32, 64 };
		const int32 ExpectedOffsets[] = { 0, 16, 48 };
		for (int32 Cascade = 0; Cascade < 3; ++Cascade)
		{
			TestEqual(TEXT("Slice ray count follows hierarchy"), Slices[Cascade].RayCount, ExpectedRays[Cascade]);
			TestEqual(TEXT("Slice offset follows packed hierarchy"), Slices[Cascade].StorageOffset, ExpectedOffsets[Cascade]);
		}
	}

	const int64 ExpectedBytes = 10ll * (112ll * 28ll + 16ll) + 112ll * 8ll;
	TestEqual(TEXT("Storage estimate reflects hierarchical ray counts"),
		Config.GetEstimatedStorageBytes(10), ExpectedBytes);

	Config.MaxTraceDistance = 1000.0f;
	TestEqual(TEXT("Trailing cascades outside trace distance are excluded"), Config.GetSafeCascadeCount(), 2);
	TestEqual(TEXT("Packed ray count follows active cascades"), Config.GetTotalRayCount(), 48);

	bool bAllCascadeCountsValid = true;
	bool bAllRayCountsHierarchical = true;
	bool bAllStorageOffsetsContiguous = true;
	bool bAllRayBudgetsValid = true;
	bool bAllDistanceIntervalsValid = true;
	const float TraceDistances[] = { 1.0f, 249.0f, 250.0f, 1000.0f, 7750.0f, 1000000.0f };
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
				const int32 TotalRays = Sweep.GetTotalRayCount();
				bAllCascadeCountsValid &= ActiveCount >= 1 && ActiveCount <= CascadeCount;
				bAllRayBudgetsValid &= TotalRays >= 1 && TotalRays <= 128;

				int32 ExpectedOffset = 0;
				for (int32 Level = 0; Level < ActiveCount; ++Level)
				{
					const int32 ExpectedLevelRays = BaseRays << Level;
					bAllRayCountsHierarchical &= Sweep.GetRayCount(Level) == ExpectedLevelRays;
					bAllStorageOffsetsContiguous &= Sweep.GetRayStorageOffset(Level) == ExpectedOffset;
					ExpectedOffset += ExpectedLevelRays;
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
				bAllRayCountsHierarchical &= TotalRays == ExpectedOffset;
			}
		}
	}

	TestTrue(TEXT("All supported cascade counts retain a valid active interval"), bAllCascadeCountsValid);
	TestTrue(TEXT("Ray counts double at each cascade"), bAllRayCountsHierarchical);
	TestTrue(TEXT("All layouts stay within the 128-ray budget"), bAllRayBudgetsValid);
	TestTrue(TEXT("All ray storage offsets are contiguous"), bAllStorageOffsetsContiguous);
	TestTrue(TEXT("All distance intervals are contiguous and reach MaxTraceDistance"), bAllDistanceIntervalsValid);

	const int64 OneProbeBytes = Config.GetEstimatedStorageBytes(1);
	TestTrue(TEXT("Storage estimate grows with probe count"), Config.GetEstimatedStorageBytes(2) > OneProbeBytes);
	TestTrue(TEXT("Storage estimate includes shared ray directions"), OneProbeBytes > Config.GetTotalRayCount() * 28ll);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
