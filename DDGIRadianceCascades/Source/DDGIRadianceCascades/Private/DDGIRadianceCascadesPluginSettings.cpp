/*
* Copyright (c) 2019-2021, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "DDGIRadianceCascadesPluginSettings.h"
#include "DDGIRadianceCascadesPlugin.h"
#include "DDGIVolumeComponent.h"

#define LOCTEXT_NAMESPACE "DDGIRadianceCascadesPlugin"

#if WITH_EDITOR

void UDDGIRadianceCascadesPluginSettings::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// tell the scene proxies about the bit depth change
	if (PropertyChangedEvent.MemberProperty)
	{
		if (PropertyChangedEvent.MemberProperty->GetFName() == GET_MEMBER_NAME_CHECKED(UDDGIRadianceCascadesPluginSettings, IrradianceBits) ||
			PropertyChangedEvent.MemberProperty->GetFName() == GET_MEMBER_NAME_CHECKED(UDDGIRadianceCascadesPluginSettings, DistanceBits))
		{
			FDDGIVolumeSceneProxy::OnIrradianceOrDistanceBitsChange();
		}
	}
}

FText UDDGIRadianceCascadesPluginSettings::GetSectionText() const
{
	return LOCTEXT("SettingsDisplayName", "DDGIRadianceCascades");
}

#endif	// WITH_EDITOR

UDDGIRadianceCascadesPluginSettings::UDDGIRadianceCascadesPluginSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName  = TEXT("DDGIRadianceCascades");
}

#undef LOCTEXT_NAMESPACE
