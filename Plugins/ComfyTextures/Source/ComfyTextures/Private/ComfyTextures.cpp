// Copyright Epic Games, Inc. All Rights Reserved.

#include "ComfyTextures.h"
#include "ISettingsModule.h"
#include "ComfyTexturesWidgetBase.h"

#define LOCTEXT_NAMESPACE "FComfyTexturesModule"

void FComfyTexturesModule::StartupModule()
{
  if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
  {
    SettingsModule->RegisterSettings("Project", "Plugins", "ComfyTextures",
      LOCTEXT("RuntimeSettingsName", "Comfy Textures"),
      LOCTEXT("RuntimeSettingsDescription", "Configure Comfy Textures"),
      GetMutableDefault<UComfyTexturesSettings>()
    );
  }
}

void FComfyTexturesModule::ShutdownModule()
{
  if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
  {
    SettingsModule->UnregisterSettings("Project", "Plugins", "ComfyTextures");
  }
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FComfyTexturesModule, ComfyTextures)