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
#include "DeferredShadingRenderer.h"
#include "HAL/IConsoleManager.h"

#define LOCTEXT_NAMESPACE "FDDGIRadianceCascadesPlugin"

namespace
{
	TSharedPtr<FDDGIRadianceCascadeRHIViewExtension, ESPMode::ThreadSafe> GDDGIRadianceCascadeViewExtension;
	FDelegateHandle GDDGIExperimentalGIDelegateHandle;

	int32 GPreviousExperimentalGIValue = 0;
	uint32 GPreviousExperimentalGISetBy = 0;
	bool bDDGIChangedExperimentalGISetting = false;
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

	if (GDDGIExperimentalGIDelegateHandle.IsValid())
	{
		FGlobalIlluminationExperimentalPluginDelegates::RenderDiffuseIndirectLight().Remove(GDDGIExperimentalGIDelegateHandle);
		GDDGIExperimentalGIDelegateHandle.Reset();
	}

	// Undo only the value this module changed. Do not overwrite a setting that
	// another system or the user changed while DDGI was loaded.
	if (bDDGIChangedExperimentalGISetting)
	{
		if (IConsoleVariable* ExperimentalGICVar =
			IConsoleManager::Get().FindConsoleVariable(TEXT("r.GlobalIllumination.ExperimentalPlugin")))
		{
			const uint32 CurrentSetBy = static_cast<uint32>(ExperimentalGICVar->GetFlags() & ECVF_SetByMask);
			if (ExperimentalGICVar->GetInt() == 1 &&
				CurrentSetBy == static_cast<uint32>(ECVF_SetByCode))
			{
				ExperimentalGICVar->Set(
					GPreviousExperimentalGIValue,
					static_cast<EConsoleVariableFlags>(GPreviousExperimentalGISetBy));
			}
		}
		bDDGIChangedExperimentalGISetting = false;
	}

	GDDGIRadianceCascadeViewExtension.Reset();
}

void FDDGIRadianceCascadesPlugin::RegisterViewExtension()
{
	if (!GDDGIRadianceCascadeViewExtension.IsValid())
	{
		GDDGIRadianceCascadeViewExtension =
			FSceneViewExtensions::NewExtension<FDDGIRadianceCascadeRHIViewExtension>();
		UE_LOG(LogTemp, Warning, TEXT("DDGI DIAG: RHI view extension registered after engine initialization."));
	}

	if (!GDDGIExperimentalGIDelegateHandle.IsValid())
	{
		GDDGIExperimentalGIDelegateHandle =
			FGlobalIlluminationExperimentalPluginDelegates::RenderDiffuseIndirectLight().AddStatic(
				&FDDGIRadianceCascadeRHIViewExtension::RenderDiffuseIndirect);
	}

	// UE4.27 only broadcasts the experimental GI delegate when this renderer
	// switch is enabled. r.DDGI remains the actual per-frame DDGI on/off switch.
	if (IConsoleVariable* ExperimentalGICVar =
		IConsoleManager::Get().FindConsoleVariable(TEXT("r.GlobalIllumination.ExperimentalPlugin")))
	{
		if (!bDDGIChangedExperimentalGISetting && ExperimentalGICVar->GetInt() != 1)
		{
			GPreviousExperimentalGIValue = ExperimentalGICVar->GetInt();
			GPreviousExperimentalGISetBy = static_cast<uint32>(
				ExperimentalGICVar->GetFlags() & ECVF_SetByMask);
			ExperimentalGICVar->Set(1, ECVF_SetByCode);

			const uint32 CurrentSetBy = static_cast<uint32>(
				ExperimentalGICVar->GetFlags() & ECVF_SetByMask);
			bDDGIChangedExperimentalGISetting =
				ExperimentalGICVar->GetInt() == 1 &&
				CurrentSetBy == static_cast<uint32>(ECVF_SetByCode);
		}

		if (ExperimentalGICVar->GetInt() == 1)
		{
			UE_LOG(LogTemp, Warning, TEXT("DDGI DIAG: UE4.27 experimental GI plugin dispatch is enabled."));
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("DDGI DIAG: r.GlobalIllumination.ExperimentalPlugin is disabled by a higher-priority setting; DDGI apply callback will not run."));
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("DDGI DIAG: r.GlobalIllumination.ExperimentalPlugin was not registered; DDGI apply callback will not run."));
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FDDGIRadianceCascadesPlugin, DDGIRadianceCascades)
