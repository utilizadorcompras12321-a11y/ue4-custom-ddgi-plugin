/*
* Copyright (c) 2019-2021, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "DDGIRadianceCascadesPlugin.h"

#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "DDGIRadianceCascadeRHIViewExtension.h"
#include "SceneViewExtension.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FDDGIRadianceCascadesPlugin"

namespace
{
	TSharedPtr<FDDGIRadianceCascadeRHIViewExtension, ESPMode::ThreadSafe> GDDGIRadianceCascadeViewExtension;
}

void FDDGIRadianceCascadesPlugin::StartupModule()
{
	// Get the base directory of this plugin
	FString BaseDir = IPluginManager::Get().FindPlugin(GetModularFeatureName())->GetBaseDir();

	// Register the shader directory
	FString PluginShaderDir = FPaths::Combine(BaseDir, TEXT("Shaders"));
	FString PluginMapping = TEXT("/Plugin/") + GetModularFeatureName();
	AddShaderSourceDirectoryMapping(PluginMapping, PluginShaderDir);


	// PostConfigInit runs before GEngine's view-extension registry is guaranteed to exist.
	// Keep shader mapping early, but defer extension registration until UEngine::Init completes.
	PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(
		this, &FDDGIRadianceCascadesPlugin::RegisterViewExtension);
}

void FDDGIRadianceCascadesPlugin::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.

	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}

	GDDGIRadianceCascadeViewExtension.Reset();
}

void FDDGIRadianceCascadesPlugin::RegisterViewExtension()
{
	if (!GDDGIRadianceCascadeViewExtension.IsValid())
	{
		GDDGIRadianceCascadeViewExtension =
			FSceneViewExtensions::NewExtension<FDDGIRadianceCascadeRHIViewExtension>();
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FDDGIRadianceCascadesPlugin, DDGIRadianceCascades)
