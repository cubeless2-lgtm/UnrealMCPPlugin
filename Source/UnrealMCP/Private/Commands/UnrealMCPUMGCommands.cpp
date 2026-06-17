#include "Commands/UnrealMCPUMGCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "JsonObjectConverter.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditor.h"

namespace
{
constexpr const TCHAR* DefaultSafeWidgetRoot = TEXT("/Game/_MCP_Temp/DurableSaveGate/UserWidgetActual");
constexpr const TCHAR* SafeTempRoot = TEXT("/Game/_MCP_Temp/");

bool TryGetStringFieldAny(const TSharedPtr<FJsonObject>& Params, const TArray<FString>& FieldNames, FString& OutValue)
{
	if (!Params.IsValid())
	{
		return false;
	}

	for (const FString& FieldName : FieldNames)
	{
		if (Params->TryGetStringField(FieldName, OutValue) && !OutValue.TrimStartAndEnd().IsEmpty())
		{
			OutValue.TrimStartAndEndInline();
			OutValue.TrimQuotesInline();
			return true;
		}
	}

	return false;
}

bool IsSafeTempPackagePath(const FString& PackageName)
{
	return PackageName == TEXT("/Game/_MCP_Temp") || PackageName.StartsWith(SafeTempRoot);
}

FString NormalizePackagePath(FString PackagePath)
{
	PackagePath.TrimStartAndEndInline();
	PackagePath.TrimQuotesInline();
	while (PackagePath.EndsWith(TEXT("/")))
	{
		PackagePath.LeftChopInline(1);
	}
	return PackagePath.IsEmpty() ? FString(DefaultSafeWidgetRoot) : PackagePath;
}

FString NormalizeObjectPath(FString ObjectPath)
{
	ObjectPath = FPackageName::ExportTextPathToObjectPath(ObjectPath).TrimStartAndEnd();
	ObjectPath.TrimQuotesInline();
	if ((ObjectPath.StartsWith(TEXT("/Game/")) || ObjectPath.StartsWith(TEXT("/Engine/"))) && !ObjectPath.Contains(TEXT(".")))
	{
		const FString AssetName = FPackageName::GetShortName(ObjectPath);
		ObjectPath = FString::Printf(TEXT("%s.%s"), *ObjectPath, *AssetName);
	}
	return ObjectPath;
}

FString JoinPackagePath(const FString& PackagePath, const FString& AssetName)
{
	return FString::Printf(TEXT("%s/%s"), *NormalizePackagePath(PackagePath), *AssetName);
}

bool ValidateSafePackageName(const FString& PackageName, const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	FText InvalidReason;
	if (PackageName.IsEmpty() || PackageName.Contains(TEXT("//")) || !FPackageName::IsValidLongPackageName(PackageName, true, &InvalidReason))
	{
		OutError = FString::Printf(TEXT("Invalid package path '%s': %s"), *PackageName, *InvalidReason.ToString());
		return false;
	}

	bool bAllowProductionPath = false;
	Params->TryGetBoolField(TEXT("allow_production_path"), bAllowProductionPath);
	if (!bAllowProductionPath && !IsSafeTempPackagePath(PackageName))
	{
		OutError = FString::Printf(TEXT("UMG durable route blocked production path '%s'; use /Game/_MCP_Temp/... or set allow_production_path=true explicitly"), *PackageName);
		return false;
	}

	return true;
}

bool ResolveWidgetBlueprintObjectPath(const TSharedPtr<FJsonObject>& Params, FString& OutObjectPath, FString& OutPackageName, FString& OutError)
{
	FString ExplicitPath;
	if (TryGetStringFieldAny(Params, {TEXT("blueprint_path"), TEXT("target_asset_path"), TEXT("asset_path")}, ExplicitPath))
	{
		OutObjectPath = NormalizeObjectPath(ExplicitPath);
		OutPackageName = FPackageName::ObjectPathToPackageName(OutObjectPath);
		return ValidateSafePackageName(OutPackageName, Params, OutError);
	}

	FString BlueprintName;
	if (!TryGetStringFieldAny(Params, {TEXT("blueprint_name"), TEXT("widget_blueprint_name"), TEXT("widget_name"), TEXT("name")}, BlueprintName))
	{
		OutError = TEXT("Missing widget blueprint target; provide blueprint_path, blueprint_name, widget_blueprint_name, widget_name, or name");
		return false;
	}

	if (BlueprintName.StartsWith(TEXT("/Game/")) || BlueprintName.StartsWith(TEXT("/Engine/")))
	{
		OutObjectPath = NormalizeObjectPath(BlueprintName);
		OutPackageName = FPackageName::ObjectPathToPackageName(OutObjectPath);
		return ValidateSafePackageName(OutPackageName, Params, OutError);
	}

	FString PackageRoot;
	if (!Params->TryGetStringField(TEXT("path"), PackageRoot))
	{
		PackageRoot = DefaultSafeWidgetRoot;
	}
	const FString PackagePath = JoinPackagePath(PackageRoot, BlueprintName);
	OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *BlueprintName);
	OutPackageName = PackagePath;
	return ValidateSafePackageName(OutPackageName, Params, OutError);
}

bool ResolveCreateTarget(const TSharedPtr<FJsonObject>& Params, FString& OutAssetName, FString& OutPackageName, FString& OutObjectPath, FString& OutError)
{
	if (!TryGetStringFieldAny(Params, {TEXT("name"), TEXT("widget_name"), TEXT("blueprint_name"), TEXT("widget_blueprint_name")}, OutAssetName))
	{
		OutError = TEXT("Missing widget blueprint name; provide name or widget_name");
		return false;
	}

	if (OutAssetName.StartsWith(TEXT("/Game/")) || OutAssetName.StartsWith(TEXT("/Engine/")))
	{
		OutObjectPath = NormalizeObjectPath(OutAssetName);
		OutPackageName = FPackageName::ObjectPathToPackageName(OutObjectPath);
		OutAssetName = FPackageName::GetShortName(OutPackageName);
		return ValidateSafePackageName(OutPackageName, Params, OutError);
	}

	FString PackageRoot;
	if (!Params->TryGetStringField(TEXT("path"), PackageRoot))
	{
		PackageRoot = DefaultSafeWidgetRoot;
	}
	OutPackageName = JoinPackagePath(PackageRoot, OutAssetName);
	OutObjectPath = FString::Printf(TEXT("%s.%s"), *OutPackageName, *OutAssetName);
	return ValidateSafePackageName(OutPackageName, Params, OutError);
}

bool TryGetChildWidgetName(const TSharedPtr<FJsonObject>& Params, const TArray<FString>& PreferredNames, FString& OutWidgetName)
{
	if (TryGetStringFieldAny(Params, PreferredNames, OutWidgetName))
	{
		return true;
	}

	if (Params->HasField(TEXT("blueprint_name")) || Params->HasField(TEXT("blueprint_path")) || Params->HasField(TEXT("target_asset_path")))
	{
		return Params->TryGetStringField(TEXT("widget_name"), OutWidgetName);
	}

	return false;
}

UWidgetBlueprint* LoadWidgetBlueprintForParams(const TSharedPtr<FJsonObject>& Params, FString& OutObjectPath, FString& OutPackageName, FString& OutError)
{
	if (!ResolveWidgetBlueprintObjectPath(Params, OutObjectPath, OutPackageName, OutError))
	{
		return nullptr;
	}

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(OutObjectPath);
	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(LoadedAsset);
	if (!WidgetBlueprint)
	{
		OutError = FString::Printf(TEXT("Failed to load Widget Blueprint: %s"), *OutObjectPath);
		return nullptr;
	}
	if (!WidgetBlueprint->WidgetTree)
	{
		OutError = FString::Printf(TEXT("Widget Blueprint has no WidgetTree: %s"), *OutObjectPath);
		return nullptr;
	}

	return WidgetBlueprint;
}

UCanvasPanel* GetOrCreateRootCanvas(UWidgetBlueprint* WidgetBlueprint, bool bCreateIfMissing, bool& bOutRootCreated, FString& OutError)
{
	bOutRootCreated = false;
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		OutError = TEXT("Widget Blueprint has no WidgetTree");
		return nullptr;
	}

	if (!WidgetBlueprint->WidgetTree->RootWidget && bCreateIfMissing)
	{
		WidgetBlueprint->Modify();
		WidgetBlueprint->WidgetTree->Modify();
		UCanvasPanel* RootCanvas = WidgetBlueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
		WidgetBlueprint->WidgetTree->RootWidget = RootCanvas;
		bOutRootCreated = RootCanvas != nullptr;
	}

	UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(WidgetBlueprint->WidgetTree->RootWidget);
	if (!RootCanvas)
	{
		OutError = TEXT("Root widget is not a Canvas Panel");
	}
	return RootCanvas;
}

FVector2D ReadPosition(const TSharedPtr<FJsonObject>& Params)
{
	FVector2D Position(0.0f, 0.0f);
	const TArray<TSharedPtr<FJsonValue>>* PosArray = nullptr;
	if (Params->TryGetArrayField(TEXT("position"), PosArray) && PosArray && PosArray->Num() >= 2)
	{
		Position.X = (*PosArray)[0]->AsNumber();
		Position.Y = (*PosArray)[1]->AsNumber();
	}
	return Position;
}

void FinalizeWidgetBlueprintMutation(const TSharedPtr<FJsonObject>& Params, UWidgetBlueprint* WidgetBlueprint, const FString& ObjectPath, TSharedPtr<FJsonObject>& Response)
{
	bool bRequestCompile = true;
	Params->TryGetBoolField(TEXT("request_compile"), bRequestCompile);
	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
	Response->SetBoolField(TEXT("compile_executed"), false);
	Response->SetBoolField(TEXT("save_executed"), false);
	Response->SetBoolField(TEXT("save_requested"), bSave);
	Response->SetBoolField(TEXT("production_path_write_allowed"), false);
	Response->SetStringField(TEXT("blueprint_path"), ObjectPath);

	if (bRequestCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		Response->SetBoolField(TEXT("compile_executed"), true);
	}

	if (bSave)
	{
		Response->SetBoolField(TEXT("save_succeeded"), UEditorAssetLibrary::SaveAsset(ObjectPath, false));
		Response->SetBoolField(TEXT("save_executed"), true);
	}
}
}

FUnrealMCPUMGCommands::FUnrealMCPUMGCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleCommand(const FString& CommandName, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandName == TEXT("create_umg_widget_blueprint"))
	{
		return HandleCreateUMGWidgetBlueprint(Params);
	}
	else if (CommandName == TEXT("add_text_block_to_widget"))
	{
		return HandleAddTextBlockToWidget(Params);
	}
	else if (CommandName == TEXT("add_widget_to_viewport"))
	{
		return HandleAddWidgetToViewport(Params);
	}
	else if (CommandName == TEXT("add_button_to_widget"))
	{
		return HandleAddButtonToWidget(Params);
	}
	else if (CommandName == TEXT("bind_widget_event"))
	{
		return HandleBindWidgetEvent(Params);
	}
	else if (CommandName == TEXT("set_text_block_binding"))
	{
		return HandleSetTextBlockBinding(Params);
	}

	return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown UMG command: %s"), *CommandName));
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleCreateUMGWidgetBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetName;
	FString PackageName;
	FString ObjectPath;
	FString Error;
	if (!ResolveCreateTarget(Params, AssetName, PackageName, ObjectPath, Error))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
	}

	if (UEditorAssetLibrary::DoesAssetExist(ObjectPath))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Widget Blueprint already exists: %s"), *ObjectPath));
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create package"));
	}

	UBlueprint* NewBlueprint = FKismetEditorUtilities::CreateBlueprint(
		UUserWidget::StaticClass(),
		Package,
		FName(*AssetName),
		BPTYPE_Normal,
		UWidgetBlueprint::StaticClass(),
		UWidgetBlueprintGeneratedClass::StaticClass(),
		FName("CreateUMGWidget")
	);

	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(NewBlueprint);
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create Widget Blueprint"));
	}

	bool bRootCreated = false;
	UCanvasPanel* RootCanvas = GetOrCreateRootCanvas(WidgetBlueprint, true, bRootCreated, Error);
	if (!RootCanvas)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
	}

	FAssetRegistryModule::AssetCreated(WidgetBlueprint);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("name"), AssetName);
	ResultObj->SetStringField(TEXT("path"), ObjectPath);
	ResultObj->SetBoolField(TEXT("root_widget_created"), bRootCreated);
	FinalizeWidgetBlueprintMutation(Params, WidgetBlueprint, ObjectPath, ResultObj);
	return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleAddTextBlockToWidget(const TSharedPtr<FJsonObject>& Params)
{
	FString ObjectPath;
	FString PackageName;
	FString Error;
	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintForParams(Params, ObjectPath, PackageName, Error);
	if (!WidgetBlueprint)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
	}

	FString TextBlockName;
	if (!TryGetChildWidgetName(Params, {TEXT("text_block_name"), TEXT("child_widget_name")}, TextBlockName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing text block widget name; provide text_block_name"));
	}

	FString InitialText = TEXT("New Text Block");
	Params->TryGetStringField(TEXT("text"), InitialText);

	bool bRootCreated = false;
	UCanvasPanel* RootCanvas = GetOrCreateRootCanvas(WidgetBlueprint, true, bRootCreated, Error);
	if (!RootCanvas)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
	}

	WidgetBlueprint->Modify();
	WidgetBlueprint->WidgetTree->Modify();
	UTextBlock* TextBlock = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), *TextBlockName);
	if (!TextBlock)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create Text Block widget"));
	}
	TextBlock->SetText(FText::FromString(InitialText));

	UCanvasPanelSlot* PanelSlot = RootCanvas->AddChildToCanvas(TextBlock);
	if (PanelSlot)
	{
		PanelSlot->SetPosition(ReadPosition(Params));
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("widget_name"), TextBlockName);
	ResultObj->SetStringField(TEXT("text"), InitialText);
	ResultObj->SetBoolField(TEXT("root_widget_created"), bRootCreated);
	ResultObj->SetBoolField(TEXT("child_widget_added"), true);
	FinalizeWidgetBlueprintMutation(Params, WidgetBlueprint, ObjectPath, ResultObj);
	return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleAddWidgetToViewport(const TSharedPtr<FJsonObject>& Params)
{
	FString ObjectPath;
	FString PackageName;
	FString Error;
	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintForParams(Params, ObjectPath, PackageName, Error);
	if (!WidgetBlueprint)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
	}

	int32 ZOrder = 0;
	Params->TryGetNumberField(TEXT("z_order"), ZOrder);
	UClass* WidgetClass = WidgetBlueprint->GeneratedClass;
	if (!WidgetClass)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get widget class"));
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("blueprint_path"), ObjectPath);
	ResultObj->SetStringField(TEXT("class_path"), WidgetClass->GetPathName());
	ResultObj->SetNumberField(TEXT("z_order"), ZOrder);
	ResultObj->SetStringField(TEXT("note"), TEXT("Widget class ready. Use CreateWidget and AddToViewport nodes in Blueprint to display in game."));
	return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleAddButtonToWidget(const TSharedPtr<FJsonObject>& Params)
{
	FString ObjectPath;
	FString PackageName;
	FString Error;
	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintForParams(Params, ObjectPath, PackageName, Error);
	if (!WidgetBlueprint)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
	}

	FString ButtonName;
	if (!TryGetChildWidgetName(Params, {TEXT("button_name"), TEXT("child_widget_name")}, ButtonName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing button widget name; provide button_name"));
	}

	FString ButtonText = TEXT("Button");
	Params->TryGetStringField(TEXT("text"), ButtonText);

	bool bRootCreated = false;
	UCanvasPanel* RootCanvas = GetOrCreateRootCanvas(WidgetBlueprint, true, bRootCreated, Error);
	if (!RootCanvas)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
	}

	WidgetBlueprint->Modify();
	WidgetBlueprint->WidgetTree->Modify();
	UButton* Button = WidgetBlueprint->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), *ButtonName);
	if (!Button)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create Button widget"));
	}

	UTextBlock* ButtonTextBlock = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), *(ButtonName + TEXT("_Text")));
	if (ButtonTextBlock)
	{
		ButtonTextBlock->SetText(FText::FromString(ButtonText));
		Button->AddChild(ButtonTextBlock);
	}

	UCanvasPanelSlot* ButtonSlot = RootCanvas->AddChildToCanvas(Button);
	if (ButtonSlot)
	{
		ButtonSlot->SetPosition(ReadPosition(Params));
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("widget_name"), ButtonName);
	ResultObj->SetBoolField(TEXT("root_widget_created"), bRootCreated);
	ResultObj->SetBoolField(TEXT("child_widget_added"), true);
	FinalizeWidgetBlueprintMutation(Params, WidgetBlueprint, ObjectPath, ResultObj);
	return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleBindWidgetEvent(const TSharedPtr<FJsonObject>& Params)
{
	FString ObjectPath;
	FString PackageName;
	FString Error;
	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintForParams(Params, ObjectPath, PackageName, Error);
	if (!WidgetBlueprint)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
	}

	FString WidgetName;
	if (!TryGetChildWidgetName(Params, {TEXT("widget_component_name"), TEXT("child_widget_name")}, WidgetName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing widget component name"));
	}

	FString EventName;
	if (!Params->TryGetStringField(TEXT("event_name"), EventName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing event_name parameter"));
	}

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(WidgetBlueprint);
	if (!EventGraph)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to find event graph"));
	}

	UWidget* Widget = WidgetBlueprint->WidgetTree->FindWidget(*WidgetName);
	if (!Widget)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to find widget: %s"), *WidgetName));
	}

	UK2Node_Event* EventNode = nullptr;
	TArray<UK2Node_Event*> AllEventNodes;
	FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_Event>(WidgetBlueprint, AllEventNodes);
	for (UK2Node_Event* Node : AllEventNodes)
	{
		if (Node && Node->CustomFunctionName == FName(*EventName) && Node->EventReference.GetMemberParentClass() == Widget->GetClass())
		{
			EventNode = Node;
			break;
		}
	}

	if (!EventNode)
	{
		FKismetEditorUtilities::CreateNewBoundEventForClass(Widget->GetClass(), FName(*EventName), WidgetBlueprint, nullptr);
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("event_name"), EventName);
	ResultObj->SetBoolField(TEXT("event_graph_mutation_performed"), true);
	FinalizeWidgetBlueprintMutation(Params, WidgetBlueprint, ObjectPath, ResultObj);
	return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleSetTextBlockBinding(const TSharedPtr<FJsonObject>& Params)
{
	FString ObjectPath;
	FString PackageName;
	FString Error;
	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintForParams(Params, ObjectPath, PackageName, Error);
	if (!WidgetBlueprint)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
	}

	FString TextBlockName;
	if (!TryGetChildWidgetName(Params, {TEXT("text_block_name"), TEXT("widget_component_name"), TEXT("child_widget_name")}, TextBlockName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing text block widget name"));
	}

	FString BindingName;
	if (!TryGetStringFieldAny(Params, {TEXT("binding_name"), TEXT("binding_property")}, BindingName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing binding_name parameter"));
	}

	UTextBlock* TextBlock = Cast<UTextBlock>(WidgetBlueprint->WidgetTree->FindWidget(FName(*TextBlockName)));
	if (!TextBlock)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to find TextBlock widget: %s"), *TextBlockName));
	}

	FBlueprintEditorUtils::AddMemberVariable(
		WidgetBlueprint,
		FName(*BindingName),
		FEdGraphPinType(UEdGraphSchema_K2::PC_Text, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType())
	);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("binding_name"), BindingName);
	ResultObj->SetBoolField(TEXT("widget_binding_mutation_performed"), true);
	FinalizeWidgetBlueprintMutation(Params, WidgetBlueprint, ObjectPath, ResultObj);
	return ResultObj;
}
