/*
* Copyright (c) 2019-2021, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "DDGIRadianceCascadesEditor.h"
#include "DDGIVolume.h"
#include "IPlacementModeModule.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyleRegistry.h"
#include "DDGIRadianceCascadesDetails.h"
#include "PropertyEditorModule.h"

#define LOCTEXT_NAMESPACE "FDDGIRadianceCascadesEditor"

TSharedPtr<FSlateStyleSet> FDDGIRadianceCascadesEditor::StyleSet;

void FDDGIRadianceCascadesEditor::StartupModule()
{
	if (!StyleSet.IsValid())
	{
		StyleSet = MakeShared<FSlateStyleSet>(FName("DDGIRadianceCascadesPlacementStyle"));
		FString IconPath = IPluginManager::Get().FindPlugin(TEXT("DDGIRadianceCascades"))->GetBaseDir() + TEXT("/Resources/Icon40.png");
		StyleSet->Set("DDGIRadianceCascadesPlacement.ModesIcon", new FSlateImageBrush(IconPath, FVector2D(40.0f, 40.0f)));

		FSlateStyleRegistry::RegisterSlateStyle(*StyleSet.Get());
	}

	IPlacementModeModule& PlacementModeModule = IPlacementModeModule::Get();
	PlacementModeModule.OnPlacementModeCategoryRefreshed().AddRaw(this, &FDDGIRadianceCascadesEditor::OnPlacementModeRefresh);

	//Get the property module
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	//Register the custom details panel we have created
	PropertyModule.RegisterCustomClassLayout(ADDGIVolume::StaticClass()->GetFName(), FOnGetDetailCustomizationInstance::CreateStatic(&FDDGIRadianceCascadesDetails::MakeInstance));
}

void FDDGIRadianceCascadesEditor::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
	if (StyleSet.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleSet.Get());
		ensure(StyleSet.IsUnique());
		StyleSet.Reset();
	}

	if (IPlacementModeModule::IsAvailable())
	{
		IPlacementModeModule::Get().OnPlacementModeCategoryRefreshed().RemoveAll(this);
	}
}

void FDDGIRadianceCascadesEditor::OnPlacementModeRefresh(FName CategoryName)
{
	static FName VolumeName = FName(TEXT("Volumes"));
	if (CategoryName == VolumeName)
	{
		FPlaceableItem* DDGIVolumePlacement = new FPlaceableItem(
			*UActorFactory::StaticClass(),
			FAssetData(ADDGIVolume::StaticClass()),
			FName("DDGIRadianceCascadesPlacement.ModesIcon"),
			TOptional<FLinearColor>(),
			TOptional<int32>(),
			FText::FromString("DDGIRadianceCascades DDGI Volume")
		);

		IPlacementModeModule::Get().RegisterPlaceableItem(CategoryName, MakeShareable(DDGIVolumePlacement));
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FDDGIRadianceCascadesEditor, DDGIRadianceCascadesEditor)
