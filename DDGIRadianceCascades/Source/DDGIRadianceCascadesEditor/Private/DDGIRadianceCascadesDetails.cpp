/*
* Copyright (c) 2019-2021, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "DDGIRadianceCascadesDetails.h"
#include "DDGIVolume.h"
#include "DDGIVolumeComponent.h"
#include "Widgets/Input/SButton.h"
#include "PropertyHandle.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "DetailCategoryBuilder.h"
#include "IDetailsView.h"

#define LOCTEXT_NAMESPACE "DDGIRadianceCascadesDetails"

TSharedRef<IDetailCustomization> FDDGIRadianceCascadesDetails::MakeInstance()
{
	return MakeShareable(new FDDGIRadianceCascadesDetails);
}

void FDDGIRadianceCascadesDetails::CustomizeDetails(IDetailLayoutBuilder& DetailLayout)
{
	const TArray< TWeakObjectPtr<UObject> >& SelectedObjects = DetailLayout.GetSelectedObjects();
	for (int32 ObjectIndex = 0; ObjectIndex < SelectedObjects.Num(); ++ObjectIndex)
	{
		const TWeakObjectPtr<UObject>& CurrentObject = SelectedObjects[ObjectIndex];
		if (CurrentObject.IsValid())
		{
			ADDGIVolume* CurrentDDGIVolumeActor = Cast<ADDGIVolume>(CurrentObject.Get());
			if (CurrentDDGIVolumeActor != NULL)
			{
				DDGIVolume = CurrentDDGIVolumeActor;
				break;
			}
		}
	}

	DetailLayout.EditCategory("GI Volume")
		.AddCustomRow(FText::FromString("Clear Probes Row"), true)
		.ValueContent()
		[
			SNew(SButton)
			.HAlign(HAlign_Center)
			.OnClicked(this, &FDDGIRadianceCascadesDetails::OnClearProbes)
			[ SNew(STextBlock).Text(FText::FromString("Clear Probes")) ]
		];
}

void FDDGIRadianceCascadesDetails::CustomizeDetails(const TSharedPtr<IDetailLayoutBuilder>& DetailBuilder)
{
	CachedDetailBuilder = DetailBuilder;
	CustomizeDetails(*DetailBuilder);
}

FReply FDDGIRadianceCascadesDetails::OnClearProbes()
{
	UDDGIVolumeComponent* DDGIComponent = DDGIVolume.IsValid() ? DDGIVolume->DDGIVolumeComponent : nullptr;
	if (DDGIComponent != nullptr)
	{
		DDGIComponent->ClearProbeData();
	}
	return FReply::Handled();
}

void FDDGIRadianceCascadesDetails::OnSourceTypeChanged()
{
	IDetailLayoutBuilder* DetailBuilder = CachedDetailBuilder.Pin().Get();
	if (DetailBuilder)
	{
		DetailBuilder->ForceRefreshDetails();
	}
}

#undef LOCTEXT_NAMESPACE
