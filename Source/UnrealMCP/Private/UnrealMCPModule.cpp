#include "UnrealMCPModule.h"
#include "UnrealMCPBridge.h"
#include "Commands/UnrealMCPMaterialCommands.h"
#include "UI/NiagaraPreviewPlayerWindow.h"
#include "Modules/ModuleManager.h"
#include "EditorSubsystem.h"
#include "Editor.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "FUnrealMCPModule"

void FUnrealMCPModule::StartupModule()
{
	UE_LOG(LogTemp, Display, TEXT("Unreal MCP Module has started"));

	if (UToolMenus::IsToolMenuUIEnabled())
	{
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FUnrealMCPModule::RegisterMenus));
	}
}

void FUnrealMCPModule::ShutdownModule()
{
	FUnrealMCPMaterialCommands::StopMaterialParameterCollectionSync();
	FNiagaraPreviewPlayerWindow::Shutdown();

	if (UToolMenus::IsToolMenuUIEnabled())
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}

	UE_LOG(LogTemp, Display, TEXT("Unreal MCP Module has shut down"));
}

void FUnrealMCPModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* MainMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu"));
	FToolMenuSection& MainSection = MainMenu->FindOrAddSection(TEXT("MCP"));
	MainSection.AddSubMenu(
		TEXT("MCP"),
		LOCTEXT("MCPMenuLabel", "MCP"),
		LOCTEXT("MCPMenuTooltip", "Open MCP editor tools."),
		FNewToolMenuDelegate::CreateLambda([](UToolMenu* MCPMenu)
		{
			FToolMenuSection& MCPSection = MCPMenu->FindOrAddSection(TEXT("MCPTools"));
			MCPSection.AddSubMenu(
				TEXT("ValidationTools"),
				LOCTEXT("MCPValidationToolsLabel", "검증도구"),
				LOCTEXT("MCPValidationToolsTooltip", "Open MCP validation and preview tools."),
				FNewToolMenuDelegate::CreateLambda([](UToolMenu* ValidationMenu)
				{
					FToolMenuSection& ValidationSection = ValidationMenu->FindOrAddSection(TEXT("Niagara"));
					ValidationSection.AddMenuEntry(
						TEXT("OpenNiagaraPreviewPlayer"),
						LOCTEXT("OpenNiagaraPreviewPlayerLabel", "나이아가라 뷰어"),
						LOCTEXT("OpenNiagaraPreviewPlayerTooltip", "Open the MCP Niagara Preview Player."),
						FSlateIcon(),
						FUIAction(FExecuteAction::CreateStatic(&FNiagaraPreviewPlayerWindow::Show)));
				}),
				false,
				FSlateIcon());
		}),
		false,
		FSlateIcon());
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FUnrealMCPModule, UnrealMCP)
