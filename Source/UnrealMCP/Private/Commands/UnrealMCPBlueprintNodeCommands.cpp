#include "Commands/UnrealMCPBlueprintNodeCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_InputAction.h"
#include "K2Node_InputAxisEvent.h"
#include "K2Node_EnhancedInputAction.h"
#include "K2Node_Self.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_AssignDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_EnumLiteral.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_MakeArray.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "GameFramework/InputSettings.h"
#include "Camera/CameraActor.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetArrayLibrary.h"
#include "EdGraphSchema_K2.h"
#include "InputAction.h"
#include "Internationalization/Text.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Class.h"

#include <initializer_list>

// Declare the log category
DEFINE_LOG_CATEGORY_STATIC(LogUnrealMCP, Log, All);

namespace
{
UClass* LoadClassForPin(const FString& ClassPathOrName);
UEnum* LoadEnumForPin(const FString& EnumPathOrName);
FString GetPinDefaultStringForType(const TSharedPtr<FJsonValue>& Value, const FEdGraphPinType& PinType);

FString GetPinDirectionName(const UEdGraphPin* Pin)
{
    return Pin && Pin->Direction == EGPD_Output ? TEXT("output") : TEXT("input");
}

FString NormalizeAssetObjectPathForLoad(const FString& AssetPath)
{
    FString NormalizedPath = FPackageName::ExportTextPathToObjectPath(AssetPath).TrimStartAndEnd();
    NormalizedPath.TrimQuotesInline();

    if ((NormalizedPath.StartsWith(TEXT("/Game/")) || NormalizedPath.StartsWith(TEXT("/Engine/"))) && !NormalizedPath.Contains(TEXT(".")))
    {
        const FString AssetName = FPackageName::GetShortName(NormalizedPath);
        NormalizedPath = FString::Printf(TEXT("%s.%s"), *NormalizedPath, *AssetName);
    }

    return NormalizedPath;
}

FString GetPinDefaultString(const TSharedPtr<FJsonValue>& Value, const UEdGraphPin* Pin)
{
    if (!Value.IsValid())
    {
        return FString();
    }

    if (Value->Type == EJson::Boolean)
    {
        return Value->AsBool() ? TEXT("true") : TEXT("false");
    }

    if (Value->Type == EJson::Number)
    {
        if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int)
        {
            return FString::FromInt(FMath::RoundToInt(Value->AsNumber()));
        }
        return FString::SanitizeFloat(Value->AsNumber());
    }

    if (Value->Type == EJson::String)
    {
        return Value->AsString();
    }

    if (Value->Type == EJson::Array || Value->Type == EJson::Object)
    {
        FString Serialized;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
        FJsonSerializer::Serialize(Value, TEXT(""), Writer);
        return Serialized;
    }

    return FString();
}

FString NormalizeTypeName(const FString& TypeName)
{
    FString Normalized = TypeName.ToLower();
    Normalized.TrimStartAndEndInline();
    Normalized.ReplaceInline(TEXT(" "), TEXT(""));
    Normalized.ReplaceInline(TEXT("_"), TEXT(""));
    Normalized.ReplaceInline(TEXT("-"), TEXT(""));
    return Normalized;
}

bool GetBoolParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, bool bDefaultValue)
{
    if (!Params.IsValid() || !Params->HasField(FieldName))
    {
        return bDefaultValue;
    }
    return Params->GetBoolField(FieldName);
}

FString GetStringParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, const FString& DefaultValue = FString())
{
    if (!Params.IsValid())
    {
        return DefaultValue;
    }

    FString Value;
    return Params->TryGetStringField(FieldName, Value) ? Value : DefaultValue;
}

FString GetEnumTypeParam(const TSharedPtr<FJsonObject>& Params, const FString& PreferredFieldName = TEXT("enum_type"))
{
    if (!Params.IsValid())
    {
        return FString();
    }

    TArray<FString> FieldNames;
    if (!PreferredFieldName.IsEmpty())
    {
        FieldNames.Add(PreferredFieldName);
    }
    FieldNames.Add(TEXT("enum_type"));
    FieldNames.Add(TEXT("type_object"));
    FieldNames.Add(TEXT("enum_path"));

    TSet<FString> SeenFields;
    for (const FString& FieldName : FieldNames)
    {
        if (FieldName.IsEmpty() || SeenFields.Contains(FieldName))
        {
            continue;
        }
        SeenFields.Add(FieldName);

        FString Value;
        if (Params->TryGetStringField(FieldName, Value))
        {
            Value.TrimStartAndEndInline();
            if (!Value.IsEmpty())
            {
                return Value;
            }
        }
    }

    return FString();
}

const UEnum* GetEnumForPinType(const FEdGraphPinType& PinType)
{
    if (PinType.PinCategory != UEdGraphSchema_K2::PC_Byte || PinType.IsArray())
    {
        return nullptr;
    }

    return Cast<UEnum>(PinType.PinSubCategoryObject.Get());
}

bool IsEnumIndexHiddenOrSpacer(const UEnum* Enum, int32 Index)
{
    return !Enum ||
        !Enum->IsValidEnumValue(Enum->GetValueByIndex(Index)) ||
        Enum->HasMetaData(TEXT("Hidden"), Index) ||
        Enum->HasMetaData(TEXT("Spacer"), Index);
}

int32 FindFirstVisibleEnumIndex(const UEnum* Enum)
{
    if (!Enum)
    {
        return INDEX_NONE;
    }

    for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
    {
        if (!IsEnumIndexHiddenOrSpacer(Enum, Index))
        {
            return Index;
        }
    }

    return INDEX_NONE;
}

TArray<TSharedPtr<FJsonValue>> EnumEntriesToJson(const UEnum* Enum, bool bSwitchCasesOnly = false)
{
    TArray<TSharedPtr<FJsonValue>> Entries;
    if (!Enum)
    {
        return Entries;
    }

    const int32 EntryCount = bSwitchCasesOnly ? FMath::Max(0, Enum->NumEnums() - 1) : Enum->NumEnums();
    for (int32 Index = 0; Index < EntryCount; ++Index)
    {
        if (IsEnumIndexHiddenOrSpacer(Enum, Index))
        {
            continue;
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Enum->GetNameStringByIndex(Index));
        Entry->SetNumberField(TEXT("value"), static_cast<double>(Enum->GetValueByIndex(Index)));
        Entry->SetStringField(TEXT("display_name"), Enum->GetDisplayNameTextByIndex(Index).ToString());
        Entries.Add(MakeShared<FJsonValueObject>(Entry));
    }

    return Entries;
}

bool ResolveEnumValueDefaultString(const TSharedPtr<FJsonValue>& Value, const UEnum* Enum, FString& OutDefaultValue, int32& OutIndex, FString& OutError)
{
    OutDefaultValue.Reset();
    OutIndex = INDEX_NONE;
    OutError.Reset();

    if (!Enum)
    {
        OutError = TEXT("Invalid enum type");
        return false;
    }

    if (!Value.IsValid())
    {
        OutIndex = FindFirstVisibleEnumIndex(Enum);
        if (OutIndex == INDEX_NONE)
        {
            OutError = FString::Printf(TEXT("Enum has no visible values: %s"), *Enum->GetPathName());
            return false;
        }
    }
    else if (Value->Type == EJson::String)
    {
        FString RequestedValue = Value->AsString();
        RequestedValue.TrimStartAndEndInline();
        RequestedValue.TrimQuotesInline();
        if (RequestedValue.IsEmpty())
        {
            OutError = TEXT("Enum value cannot be empty");
            return false;
        }

        OutIndex = Enum->GetIndexByNameString(RequestedValue);
        if (OutIndex == INDEX_NONE && !RequestedValue.Contains(TEXT("::")))
        {
            OutIndex = Enum->GetIndexByNameString(FString::Printf(TEXT("%s::%s"), *Enum->GetName(), *RequestedValue));
        }
        if (OutIndex == INDEX_NONE)
        {
            OutIndex = Enum->GetIndexByNameString(Enum->GenerateFullEnumName(*RequestedValue));
        }
    }
    else if (Value->Type == EJson::Number)
    {
        const double NumberValue = Value->AsNumber();
        const int64 IntegerValue = static_cast<int64>(NumberValue);
        if (!FMath::IsNearlyEqual(NumberValue, static_cast<double>(IntegerValue)))
        {
            OutError = TEXT("Enum numeric default must be an integer value");
            return false;
        }

        OutIndex = Enum->GetIndexByValue(IntegerValue);
    }
    else
    {
        OutError = TEXT("Enum default must be a string enum entry or integer enum value");
        return false;
    }

    if (OutIndex == INDEX_NONE)
    {
        OutError = FString::Printf(TEXT("Enum value not found for %s"), *Enum->GetPathName());
        return false;
    }
    if (IsEnumIndexHiddenOrSpacer(Enum, OutIndex))
    {
        OutError = FString::Printf(TEXT("Enum value is hidden or spacer: %s"), *Enum->GetNameStringByIndex(OutIndex));
        return false;
    }

    OutDefaultValue = Enum->GetNameStringByIndex(OutIndex);
    return true;
}

bool GetPinDefaultStringForTypeChecked(const TSharedPtr<FJsonValue>& Value, const FEdGraphPinType& PinType, FString& OutDefaultValue, FString& OutError)
{
    OutDefaultValue.Reset();
    OutError.Reset();

    if (PinType.PinCategory == UEdGraphSchema_K2::PC_Byte && Cast<UEnum>(PinType.PinSubCategoryObject.Get()) && PinType.IsArray())
    {
        OutError = TEXT("Default values for enum arrays are not supported");
        return false;
    }
    if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean && Value.IsValid() && Value->Type != EJson::Boolean)
    {
        OutError = TEXT("Boolean default values must be JSON booleans");
        return false;
    }

    if (const UEnum* Enum = GetEnumForPinType(PinType))
    {
        int32 EnumIndex = INDEX_NONE;
        return ResolveEnumValueDefaultString(Value, Enum, OutDefaultValue, EnumIndex, OutError);
    }

    OutDefaultValue = GetPinDefaultStringForType(Value, PinType);
    return true;
}

FString GetPinDefaultStringForType(const TSharedPtr<FJsonValue>& Value, const FEdGraphPinType& PinType)
{
    if (!Value.IsValid())
    {
        return FString();
    }

    const bool bIsVector = PinType.PinCategory == UEdGraphSchema_K2::PC_Struct && PinType.PinSubCategoryObject == TBaseStructure<FVector>::Get();
    const bool bIsRotator = PinType.PinCategory == UEdGraphSchema_K2::PC_Struct && PinType.PinSubCategoryObject == TBaseStructure<FRotator>::Get();
    const bool bIsTransform = PinType.PinCategory == UEdGraphSchema_K2::PC_Struct && PinType.PinSubCategoryObject == TBaseStructure<FTransform>::Get();

    if ((bIsVector || bIsRotator) && Value->Type == EJson::Array)
    {
        const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
        const double X = Values.IsValidIndex(0) ? Values[0]->AsNumber() : 0.0;
        const double Y = Values.IsValidIndex(1) ? Values[1]->AsNumber() : 0.0;
        const double Z = Values.IsValidIndex(2) ? Values[2]->AsNumber() : 0.0;
        if (bIsRotator)
        {
            return FString::Printf(TEXT("(Pitch=%s,Yaw=%s,Roll=%s)"), *FString::SanitizeFloat(X), *FString::SanitizeFloat(Y), *FString::SanitizeFloat(Z));
        }
        return FString::Printf(TEXT("(X=%s,Y=%s,Z=%s)"), *FString::SanitizeFloat(X), *FString::SanitizeFloat(Y), *FString::SanitizeFloat(Z));
    }

    if ((bIsVector || bIsRotator || bIsTransform) && Value->Type == EJson::Object)
    {
        const TSharedPtr<FJsonObject> ObjectValue = Value->AsObject();
        if (ObjectValue.IsValid())
        {
            if (bIsTransform)
            {
                const FEdGraphPinType VectorType(UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FVector>::Get(), EPinContainerType::None, false, FEdGraphTerminalType());
                const FEdGraphPinType RotatorType(UEdGraphSchema_K2::PC_Struct, NAME_None, TBaseStructure<FRotator>::Get(), EPinContainerType::None, false, FEdGraphTerminalType());

                const TSharedPtr<FJsonValue> LocationValue = ObjectValue->TryGetField(TEXT("location")).IsValid()
                    ? ObjectValue->TryGetField(TEXT("location"))
                    : ObjectValue->TryGetField(TEXT("translation"));
                const TSharedPtr<FJsonValue> RotationValue = ObjectValue->TryGetField(TEXT("rotation"));
                const TSharedPtr<FJsonValue> ScaleValue = ObjectValue->TryGetField(TEXT("scale")).IsValid()
                    ? ObjectValue->TryGetField(TEXT("scale"))
                    : ObjectValue->TryGetField(TEXT("scale3d"));

                const FString Location = LocationValue.IsValid()
                    ? GetPinDefaultStringForType(LocationValue, VectorType)
                    : TEXT("(X=0,Y=0,Z=0)");
                const FString Rotation = RotationValue.IsValid()
                    ? GetPinDefaultStringForType(RotationValue, RotatorType)
                    : TEXT("(Pitch=0,Yaw=0,Roll=0)");
                const FString Scale = ScaleValue.IsValid()
                    ? GetPinDefaultStringForType(ScaleValue, VectorType)
                    : TEXT("(X=1,Y=1,Z=1)");
                return FString::Printf(TEXT("(Rotation=%s,Translation=%s,Scale3D=%s)"), *Rotation, *Location, *Scale);
            }

            double X = 0.0;
            double Y = 0.0;
            double Z = 0.0;
            ObjectValue->TryGetNumberField(bIsRotator ? TEXT("pitch") : TEXT("x"), X);
            ObjectValue->TryGetNumberField(bIsRotator ? TEXT("yaw") : TEXT("y"), Y);
            ObjectValue->TryGetNumberField(bIsRotator ? TEXT("roll") : TEXT("z"), Z);
            if (bIsRotator)
            {
                return FString::Printf(TEXT("(Pitch=%s,Yaw=%s,Roll=%s)"), *FString::SanitizeFloat(X), *FString::SanitizeFloat(Y), *FString::SanitizeFloat(Z));
            }
            return FString::Printf(TEXT("(X=%s,Y=%s,Z=%s)"), *FString::SanitizeFloat(X), *FString::SanitizeFloat(Y), *FString::SanitizeFloat(Z));
        }
    }

    if (Value->Type == EJson::String)
    {
        return Value->AsString();
    }
    if (Value->Type == EJson::Boolean)
    {
        return Value->AsBool() ? TEXT("true") : TEXT("false");
    }
    if (Value->Type == EJson::Number)
    {
        if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int ||
            PinType.PinCategory == UEdGraphSchema_K2::PC_Int64 ||
            PinType.PinCategory == UEdGraphSchema_K2::PC_Byte)
        {
            return FString::Printf(TEXT("%lld"), static_cast<int64>(Value->AsNumber()));
        }
        return FString::SanitizeFloat(Value->AsNumber());
    }

    return GetPinDefaultString(Value, nullptr);
}

bool BuildPinTypeFromDescriptor(const FString& TypeName, const TSharedPtr<FJsonObject>& Params, const FString& ObjectFieldName, FEdGraphPinType& OutPinType, FString& OutError)
{
    const FString NormalizedType = NormalizeTypeName(TypeName);
    OutPinType = FEdGraphPinType();

    if (NormalizedType == TEXT("bool") || NormalizedType == TEXT("boolean"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    }
    else if (NormalizedType == TEXT("int") || NormalizedType == TEXT("integer"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
    }
    else if (NormalizedType == TEXT("int64") || NormalizedType == TEXT("integer64"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
    }
    else if (NormalizedType == TEXT("float") || NormalizedType == TEXT("single"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
        OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
    }
    else if (NormalizedType == TEXT("double") || NormalizedType == TEXT("real"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
        OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
    }
    else if (NormalizedType == TEXT("string"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
    }
    else if (NormalizedType == TEXT("name"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
    }
    else if (NormalizedType == TEXT("text"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
    }
    else if (NormalizedType == TEXT("vector"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
        OutPinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
    }
    else if (NormalizedType == TEXT("rotator"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
        OutPinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
    }
    else if (NormalizedType == TEXT("transform"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
        OutPinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
    }
    else if (NormalizedType == TEXT("enum") || NormalizedType == TEXT("byteenum"))
    {
        const FString EnumType = GetEnumTypeParam(Params, ObjectFieldName);
        if (EnumType.IsEmpty())
        {
            OutError = TEXT("Missing enum type parameter. Use 'type_object', 'enum_type', or 'enum_path'");
            return false;
        }

        UEnum* Enum = LoadEnumForPin(EnumType);
        if (!Enum)
        {
            OutError = FString::Printf(TEXT("Enum not found for %s type: %s"), *TypeName, *EnumType);
            return false;
        }

        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
        OutPinType.PinSubCategoryObject = Enum;
    }
    else if (NormalizedType == TEXT("object") || NormalizedType == TEXT("class"))
    {
        const FString ClassName = GetStringParam(Params, ObjectFieldName, TEXT("Object"));
        UClass* Class = LoadClassForPin(ClassName);
        if (!Class)
        {
            OutError = FString::Printf(TEXT("Class not found for %s type: %s"), *TypeName, *ClassName);
            return false;
        }
        OutPinType.PinCategory = NormalizedType == TEXT("class") ? UEdGraphSchema_K2::PC_Class : UEdGraphSchema_K2::PC_Object;
        OutPinType.PinSubCategoryObject = Class;
    }
    else
    {
        OutError = FString::Printf(TEXT("Unsupported type descriptor: %s"), *TypeName);
        return false;
    }

    if (GetBoolParam(Params, TEXT("is_array"), false))
    {
        OutPinType.ContainerType = EPinContainerType::Array;
    }

    return true;
}

TSharedPtr<FJsonObject> PinTypeToJson(const FEdGraphPinType& PinType)
{
    TSharedPtr<FJsonObject> TypeObject = MakeShared<FJsonObject>();
    TypeObject->SetStringField(TEXT("category"), PinType.PinCategory.ToString());
    TypeObject->SetStringField(TEXT("subcategory"), PinType.PinSubCategory.ToString());
    TypeObject->SetBoolField(TEXT("is_array"), PinType.IsArray());
    if (PinType.PinSubCategoryObject.IsValid())
    {
        TypeObject->SetStringField(TEXT("subcategory_object"), PinType.PinSubCategoryObject->GetPathName());
    }
    return TypeObject;
}

TSharedPtr<FUserPinInfo> FindUserDefinedPinInfo(UK2Node_EditablePinBase* EditableNode, const FName& PinName, EEdGraphPinDirection PinDirection)
{
    if (!EditableNode)
    {
        return nullptr;
    }

    for (const TSharedPtr<FUserPinInfo>& PinInfo : EditableNode->UserDefinedPins)
    {
        if (PinInfo.IsValid() && PinInfo->PinName == PinName && PinInfo->DesiredPinDirection == PinDirection)
        {
            return PinInfo;
        }
    }

    return nullptr;
}

UObject* LoadObjectForPin(const FString& ObjectPath)
{
    const FString NormalizedPath = NormalizeAssetObjectPathForLoad(ObjectPath);
    if (NormalizedPath.IsEmpty())
    {
        return nullptr;
    }

    return LoadObject<UObject>(nullptr, *NormalizedPath);
}

UClass* LoadClassForPin(const FString& ClassPathOrName)
{
    const FString NormalizedPath = NormalizeAssetObjectPathForLoad(ClassPathOrName);
    if (UClass* Class = LoadObject<UClass>(nullptr, *NormalizedPath))
    {
        return Class;
    }

    if (UClass* Class = FindFirstObject<UClass>(*ClassPathOrName))
    {
        return Class;
    }

    FString EngineClassPath = FString::Printf(TEXT("/Script/Engine.%s"), *ClassPathOrName);
    return LoadObject<UClass>(nullptr, *EngineClassPath);
}

UEnum* LoadEnumForPin(const FString& EnumPathOrName)
{
    FString RequestedName = EnumPathOrName;
    RequestedName.TrimStartAndEndInline();
    RequestedName.TrimQuotesInline();
    if (RequestedName.IsEmpty())
    {
        return nullptr;
    }

    const FString NormalizedPath = NormalizeAssetObjectPathForLoad(RequestedName);
    if (UEnum* Enum = LoadObject<UEnum>(nullptr, *NormalizedPath))
    {
        return Enum;
    }

    if (UEnum* Enum = UClass::TryFindTypeSlow<UEnum>(
        *RequestedName,
        EFindFirstObjectOptions::ExactClass | EFindFirstObjectOptions::NativeFirst))
    {
        return Enum;
    }

    if (NormalizedPath != RequestedName)
    {
        if (UEnum* Enum = UClass::TryFindTypeSlow<UEnum>(
            *NormalizedPath,
            EFindFirstObjectOptions::ExactClass | EFindFirstObjectOptions::NativeFirst))
        {
            return Enum;
        }
    }

    if (!RequestedName.Contains(TEXT("/")) && !RequestedName.Contains(TEXT(".")))
    {
        const FString EngineEnumPath = FString::Printf(TEXT("/Script/Engine.%s"), *RequestedName);
        return LoadObject<UEnum>(nullptr, *EngineEnumPath);
    }

    return nullptr;
}

UFunction* ResolveFunctionByName(UClass* OwnerClass, const FString& FunctionName, FString& OutError)
{
    OutError.Reset();
    if (!OwnerClass)
    {
        OutError = TEXT("Invalid function owner class");
        return nullptr;
    }
    if (FunctionName.IsEmpty())
    {
        OutError = TEXT("Missing function name");
        return nullptr;
    }

    if (UFunction* Function = OwnerClass->FindFunctionByName(FName(*FunctionName)))
    {
        return Function;
    }

    TArray<UFunction*> CaseInsensitiveMatches;
    for (TFieldIterator<UFunction> FunctionIt(OwnerClass, EFieldIteratorFlags::IncludeSuper); FunctionIt; ++FunctionIt)
    {
        UFunction* Candidate = *FunctionIt;
        if (Candidate && Candidate->GetName().Equals(FunctionName, ESearchCase::IgnoreCase))
        {
            CaseInsensitiveMatches.Add(Candidate);
        }
    }

    if (CaseInsensitiveMatches.Num() == 1)
    {
        return CaseInsensitiveMatches[0];
    }
    if (CaseInsensitiveMatches.Num() > 1)
    {
        OutError = FString::Printf(TEXT("Ambiguous case-insensitive function match for '%s' on class %s"), *FunctionName, *OwnerClass->GetPathName());
        return nullptr;
    }

    OutError = FString::Printf(TEXT("Function not found: %s on class %s"), *FunctionName, *OwnerClass->GetPathName());
    return nullptr;
}

bool ValidateBlueprintCallableFunction(
    UFunction* Function,
    bool bAllowLatent,
    bool bAllowEditorOnly,
    bool bAllowWildcard,
    bool bAllowDeprecated,
    bool bAllowInternal,
    FString& OutError)
{
    OutError.Reset();
    if (!Function)
    {
        OutError = TEXT("Invalid function");
        return false;
    }

    if (!Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
    {
        OutError = FString::Printf(TEXT("Function is not BlueprintCallable or BlueprintPure: %s"), *Function->GetPathName());
        return false;
    }
    if (!bAllowLatent && Function->HasMetaData(FBlueprintMetadata::MD_Latent))
    {
        OutError = FString::Printf(TEXT("Latent functions are not supported by this command unless 'allow_latent' is true: %s"), *Function->GetPathName());
        return false;
    }
    if (!bAllowEditorOnly && Function->HasAnyFunctionFlags(FUNC_EditorOnly))
    {
        OutError = FString::Printf(TEXT("Editor-only functions are not supported by this command unless 'allow_editor_only' is true: %s"), *Function->GetPathName());
        return false;
    }
    if (!bAllowDeprecated && Function->HasMetaData(FBlueprintMetadata::MD_DeprecatedFunction))
    {
        OutError = FString::Printf(TEXT("Deprecated functions are not supported by this command unless 'allow_deprecated' is true: %s"), *Function->GetPathName());
        return false;
    }
    if (!bAllowInternal &&
        (Function->HasMetaData(FBlueprintMetadata::MD_BlueprintInternalUseOnly) ||
         Function->HasMetaData(FBlueprintMetadata::MD_BlueprintInternalUseOnlyHierarchical)))
    {
        OutError = FString::Printf(TEXT("Blueprint-internal functions are not supported by this command unless 'allow_internal' is true: %s"), *Function->GetPathName());
        return false;
    }
    if (!bAllowWildcard &&
        (Function->HasMetaData(FBlueprintMetadata::MD_ArrayParam) ||
         Function->HasMetaData(FBlueprintMetadata::MD_CustomStructureParam) ||
         Function->HasMetaData(FBlueprintMetadata::MD_CustomThunk)))
    {
        OutError = FString::Printf(TEXT("Wildcard/custom thunk functions are not supported by this command unless 'allow_wildcard' is true: %s"), *Function->GetPathName());
        return false;
    }

    return true;
}

bool ApplyPinDefaultValueChecked(UEdGraphPin* Pin, const TSharedPtr<FJsonValue>& Value, const UEdGraphSchema_K2* K2Schema, FString& OutAppliedValue, FString& OutError)
{
    OutAppliedValue.Reset();
    OutError.Reset();

    if (!Pin || !Value.IsValid() || !K2Schema)
    {
        OutError = TEXT("Invalid pin default request");
        return false;
    }

    const FString PreviousDefaultValue = Pin->DefaultValue;
    UObject* PreviousDefaultObject = Pin->DefaultObject;
    const FText PreviousDefaultTextValue = Pin->DefaultTextValue;
    FString RequestedDefaultValue;
    UObject* RequestedDefaultObject = nullptr;
    bool bRequestedDefaultValue = false;
    bool bRequestedDefaultObject = false;

    if (Value->Type == EJson::String && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class)
    {
        UClass* Class = LoadClassForPin(Value->AsString());
        if (!Class)
        {
            OutError = FString::Printf(TEXT("Class not found: %s"), *Value->AsString());
            return false;
        }
        RequestedDefaultObject = Class;
        bRequestedDefaultObject = true;
        K2Schema->TrySetDefaultObject(*Pin, Class);
        OutAppliedValue = Class->GetPathName();
    }
    else if (Value->Type == EJson::String && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
    {
        UObject* Object = LoadObjectForPin(Value->AsString());
        if (!Object)
        {
            OutError = FString::Printf(TEXT("Object not found: %s"), *Value->AsString());
            return false;
        }
        RequestedDefaultObject = Object;
        bRequestedDefaultObject = true;
        K2Schema->TrySetDefaultObject(*Pin, Object);
        OutAppliedValue = Object->GetPathName();
    }
    else
    {
        FString DefaultResolveError;
        if (!GetPinDefaultStringForTypeChecked(Value, Pin->PinType, RequestedDefaultValue, DefaultResolveError))
        {
            OutError = DefaultResolveError;
            return false;
        }
        bRequestedDefaultValue = true;
        K2Schema->TrySetDefaultValue(*Pin, RequestedDefaultValue);
        OutAppliedValue = RequestedDefaultValue;
    }

    const FString DefaultError = K2Schema->IsCurrentPinDefaultValid(Pin);
    const bool bDefaultValueApplied = !bRequestedDefaultValue || K2Schema->DoesDefaultValueMatch(*Pin, RequestedDefaultValue);
    const bool bDefaultObjectApplied = !bRequestedDefaultObject || Pin->DefaultObject == RequestedDefaultObject;
    if (!DefaultError.IsEmpty() || !bDefaultValueApplied || !bDefaultObjectApplied)
    {
        Pin->DefaultValue = PreviousDefaultValue;
        Pin->DefaultObject = PreviousDefaultObject;
        Pin->DefaultTextValue = PreviousDefaultTextValue;
        OutError = DefaultError.IsEmpty()
            ? FString::Printf(TEXT("Requested default was not accepted. requested='%s', actual='%s'"), *RequestedDefaultValue, *Pin->GetDefaultAsString())
            : DefaultError;
        return false;
    }

    return true;
}

UEdGraphNode* FindNodeById(UEdGraph* Graph, const FString& NodeId)
{
    if (!Graph)
    {
        return nullptr;
    }

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Node && Node->NodeGuid.ToString() == NodeId)
        {
            return Node;
        }
    }

    return nullptr;
}

TSharedPtr<FJsonObject> PinToJson(UEdGraphPin* Pin)
{
    TSharedPtr<FJsonObject> PinObject = MakeShared<FJsonObject>();
    PinObject->SetStringField(TEXT("name"), Pin ? Pin->PinName.ToString() : FString());
    PinObject->SetStringField(TEXT("direction"), GetPinDirectionName(Pin));

    if (Pin)
    {
        PinObject->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
        PinObject->SetStringField(TEXT("subcategory"), Pin->PinType.PinSubCategory.ToString());
        PinObject->SetBoolField(TEXT("is_array"), Pin->PinType.IsArray());
        PinObject->SetObjectField(TEXT("pin_type"), PinTypeToJson(Pin->PinType));
        PinObject->SetStringField(TEXT("default_value"), Pin->DefaultValue);

        if (Pin->PinType.PinSubCategoryObject.IsValid())
        {
            PinObject->SetStringField(TEXT("subcategory_object"), Pin->PinType.PinSubCategoryObject->GetPathName());
        }
        if (Pin->DefaultObject)
        {
            PinObject->SetStringField(TEXT("default_object"), Pin->DefaultObject->GetPathName());
        }

        TArray<TSharedPtr<FJsonValue>> LinkedPins;
        for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
        {
            if (!LinkedPin || !LinkedPin->GetOwningNode())
            {
                continue;
            }

            TSharedPtr<FJsonObject> LinkedPinObject = MakeShared<FJsonObject>();
            LinkedPinObject->SetStringField(TEXT("node_id"), LinkedPin->GetOwningNode()->NodeGuid.ToString());
            LinkedPinObject->SetStringField(TEXT("node_name"), LinkedPin->GetOwningNode()->GetName());
            LinkedPinObject->SetStringField(TEXT("pin_name"), LinkedPin->PinName.ToString());
            LinkedPins.Add(MakeShared<FJsonValueObject>(LinkedPinObject));
        }
        PinObject->SetArrayField(TEXT("linked_to"), LinkedPins);
    }

    return PinObject;
}

TSharedPtr<FJsonObject> NodeToJson(UEdGraphNode* Node, bool bIncludePins)
{
    TSharedPtr<FJsonObject> NodeObject = MakeShared<FJsonObject>();
    if (!Node)
    {
        return NodeObject;
    }

    NodeObject->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
    NodeObject->SetStringField(TEXT("name"), Node->GetName());
    NodeObject->SetStringField(TEXT("class"), Node->GetClass()->GetName());
    NodeObject->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
    NodeObject->SetNumberField(TEXT("x"), Node->NodePosX);
    NodeObject->SetNumberField(TEXT("y"), Node->NodePosY);

    if (bIncludePins)
    {
        TArray<TSharedPtr<FJsonValue>> Pins;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            Pins.Add(MakeShared<FJsonValueObject>(PinToJson(Pin)));
        }
        NodeObject->SetArrayField(TEXT("pins"), Pins);
    }

    return NodeObject;
}

bool MatchesNodeFilter(UEdGraphNode* Node, const FString& NodeType, const FString& TitleContains)
{
    if (!Node)
    {
        return false;
    }

    if (!NodeType.IsEmpty())
    {
        const FString ClassName = Node->GetClass()->GetName();
        const FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
        if (!ClassName.Contains(NodeType) && !NodeTitle.Contains(NodeType))
        {
            return false;
        }
    }

    if (!TitleContains.IsEmpty() && !Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString().Contains(TitleContains))
    {
        return false;
    }

    return true;
}

FName GetMathFunctionName(const FString& Operation)
{
    if (Operation.Equals(TEXT("add"), ESearchCase::IgnoreCase) || Operation.Equals(TEXT("+"), ESearchCase::IgnoreCase))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Add_DoubleDouble);
    }
    if (Operation.Equals(TEXT("subtract"), ESearchCase::IgnoreCase) || Operation.Equals(TEXT("-"), ESearchCase::IgnoreCase))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Subtract_DoubleDouble);
    }
    if (Operation.Equals(TEXT("multiply"), ESearchCase::IgnoreCase) || Operation.Equals(TEXT("*"), ESearchCase::IgnoreCase))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Multiply_DoubleDouble);
    }
    if (Operation.Equals(TEXT("divide"), ESearchCase::IgnoreCase) || Operation.Equals(TEXT("/"), ESearchCase::IgnoreCase))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Divide_DoubleDouble);
    }
    if (Operation.Equals(TEXT("clamp"), ESearchCase::IgnoreCase))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, FClamp);
    }
    if (Operation.Equals(TEXT("in_range"), ESearchCase::IgnoreCase) || Operation.Equals(TEXT("inrange"), ESearchCase::IgnoreCase))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, InRange_FloatFloat);
    }

    return NAME_None;
}

bool IsOperationAlias(const FString& Operation, std::initializer_list<const TCHAR*> Aliases)
{
    FString NormalizedOperation = Operation.ToLower();
    NormalizedOperation.TrimStartAndEndInline();
    NormalizedOperation.ReplaceInline(TEXT(" "), TEXT("_"));
    NormalizedOperation.ReplaceInline(TEXT("-"), TEXT("_"));

    for (const TCHAR* Alias : Aliases)
    {
        FString NormalizedAlias(Alias);
        NormalizedAlias.ToLowerInline();
        NormalizedAlias.ReplaceInline(TEXT(" "), TEXT("_"));
        NormalizedAlias.ReplaceInline(TEXT("-"), TEXT("_"));
        if (NormalizedOperation == NormalizedAlias)
        {
            return true;
        }
    }

    return false;
}

FName GetCompareFunctionName(const FString& Operation, const FString& ValueType, FString& OutError)
{
    const FString NormalizedType = NormalizeTypeName(ValueType);
    const bool bLess = IsOperationAlias(Operation, {TEXT("less"), TEXT("lt"), TEXT("<")});
    const bool bGreater = IsOperationAlias(Operation, {TEXT("greater"), TEXT("gt"), TEXT(">")});
    const bool bLessEqual = IsOperationAlias(Operation, {TEXT("less_equal"), TEXT("lessequal"), TEXT("le"), TEXT("<=")});
    const bool bGreaterEqual = IsOperationAlias(Operation, {TEXT("greater_equal"), TEXT("greaterequal"), TEXT("ge"), TEXT(">=")});
    const bool bEqual = IsOperationAlias(Operation, {TEXT("equal"), TEXT("equals"), TEXT("eq"), TEXT("==")});
    const bool bNotEqual = IsOperationAlias(Operation, {TEXT("not_equal"), TEXT("notequal"), TEXT("ne"), TEXT("!=")});

    if (!bLess && !bGreater && !bLessEqual && !bGreaterEqual && !bEqual && !bNotEqual)
    {
        OutError = FString::Printf(TEXT("Unsupported compare operation: %s"), *Operation);
        return NAME_None;
    }

    if (NormalizedType == TEXT("int") || NormalizedType == TEXT("integer"))
    {
        if (bLess) { return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Less_IntInt); }
        if (bGreater) { return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Greater_IntInt); }
        if (bLessEqual) { return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, LessEqual_IntInt); }
        if (bGreaterEqual) { return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, GreaterEqual_IntInt); }
        if (bEqual) { return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_IntInt); }
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, NotEqual_IntInt);
    }
    if (NormalizedType == TEXT("int64") || NormalizedType == TEXT("integer64"))
    {
        if (bLess) { return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Less_Int64Int64); }
        if (bGreater) { return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Greater_Int64Int64); }
        if (bLessEqual) { return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, LessEqual_Int64Int64); }
        if (bGreaterEqual) { return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, GreaterEqual_Int64Int64); }
        if (bEqual) { return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_Int64Int64); }
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, NotEqual_Int64Int64);
    }
    if (NormalizedType == TEXT("float") || NormalizedType == TEXT("double") || NormalizedType == TEXT("real"))
    {
        if (bLess) { return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Less_DoubleDouble); }
        if (bGreater) { return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Greater_DoubleDouble); }
        if (bLessEqual) { return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, LessEqual_DoubleDouble); }
        if (bGreaterEqual) { return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, GreaterEqual_DoubleDouble); }
        if (bEqual) { return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_DoubleDouble); }
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, NotEqual_DoubleDouble);
    }

    if (bEqual || bNotEqual)
    {
        if (NormalizedType == TEXT("bool") || NormalizedType == TEXT("boolean"))
        {
            return bEqual
                ? GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_BoolBool)
                : GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, NotEqual_BoolBool);
        }
        if (NormalizedType == TEXT("object"))
        {
            return bEqual
                ? GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_ObjectObject)
                : GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, NotEqual_ObjectObject);
        }
        if (NormalizedType == TEXT("class"))
        {
            return bEqual
                ? GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_ClassClass)
                : GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, NotEqual_ClassClass);
        }
        if (NormalizedType == TEXT("name"))
        {
            return bEqual
                ? GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_NameName)
                : GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, NotEqual_NameName);
        }
    }

    OutError = FString::Printf(TEXT("Unsupported compare type/operation combination: type=%s, operation=%s"), *ValueType, *Operation);
    return NAME_None;
}

FName GetBooleanFunctionName(const FString& Operation, FString& OutError)
{
    if (IsOperationAlias(Operation, {TEXT("not"), TEXT("!")}))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Not_PreBool);
    }
    if (IsOperationAlias(Operation, {TEXT("and"), TEXT("&&")}))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, BooleanAND);
    }
    if (IsOperationAlias(Operation, {TEXT("nand")}))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, BooleanNAND);
    }
    if (IsOperationAlias(Operation, {TEXT("or"), TEXT("||")}))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, BooleanOR);
    }
    if (IsOperationAlias(Operation, {TEXT("xor")}))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, BooleanXOR);
    }
    if (IsOperationAlias(Operation, {TEXT("nor")}))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, BooleanNOR);
    }
    if (IsOperationAlias(Operation, {TEXT("equal"), TEXT("equals"), TEXT("eq"), TEXT("==")}))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_BoolBool);
    }
    if (IsOperationAlias(Operation, {TEXT("not_equal"), TEXT("notequal"), TEXT("ne"), TEXT("!=")}))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, NotEqual_BoolBool);
    }

    OutError = FString::Printf(TEXT("Unsupported boolean operation: %s"), *Operation);
    return NAME_None;
}

FName GetSelectFunctionName(const FString& ValueType, FString& OutError)
{
    const FString NormalizedType = NormalizeTypeName(ValueType);
    if (NormalizedType == TEXT("string"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, SelectString);
    }
    if (NormalizedType == TEXT("text"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, SelectText);
    }
    if (NormalizedType == TEXT("name"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, SelectName);
    }
    if (NormalizedType == TEXT("int") || NormalizedType == TEXT("integer"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, SelectInt);
    }
    if (NormalizedType == TEXT("float") || NormalizedType == TEXT("double") || NormalizedType == TEXT("real"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, SelectFloat);
    }
    if (NormalizedType == TEXT("vector"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, SelectVector);
    }
    if (NormalizedType == TEXT("rotator"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, SelectRotator);
    }
    if (NormalizedType == TEXT("color") || NormalizedType == TEXT("linearcolor"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, SelectColor);
    }
    if (NormalizedType == TEXT("transform"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, SelectTransform);
    }
    if (NormalizedType == TEXT("object"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, SelectObject);
    }
    if (NormalizedType == TEXT("class"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, SelectClass);
    }

    OutError = FString::Printf(TEXT("Unsupported select value type: %s"), *ValueType);
    return NAME_None;
}

FName GetIsValidFunctionName(const FString& ValueType, FString& OutError)
{
    const FString NormalizedType = NormalizeTypeName(ValueType);
    if (NormalizedType.IsEmpty() || NormalizedType == TEXT("object"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, IsValid);
    }
    if (NormalizedType == TEXT("class"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, IsValidClass);
    }

    OutError = FString::Printf(TEXT("Unsupported is_valid value type: %s"), *ValueType);
    return NAME_None;
}

FName GetLiteralFunctionName(const FString& LiteralType, FString& OutError)
{
    const FString NormalizedType = NormalizeTypeName(LiteralType);
    if (NormalizedType == TEXT("int") || NormalizedType == TEXT("integer"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralInt);
    }
    if (NormalizedType == TEXT("int64") || NormalizedType == TEXT("integer64"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralInt64);
    }
    if (NormalizedType == TEXT("float") || NormalizedType == TEXT("single"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralFloat);
    }
    if (NormalizedType == TEXT("double") || NormalizedType == TEXT("real"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralDouble);
    }
    if (NormalizedType == TEXT("bool") || NormalizedType == TEXT("boolean"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralBool);
    }
    if (NormalizedType == TEXT("name"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralName);
    }
    if (NormalizedType == TEXT("byte"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralByte);
    }
    if (NormalizedType == TEXT("string"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralString);
    }
    if (NormalizedType == TEXT("text"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, MakeLiteralText);
    }

    OutError = FString::Printf(TEXT("Unsupported literal type: %s"), *LiteralType);
    return NAME_None;
}

FName GetStructHelperFunctionName(const FString& Helper, const FString& StructType, FString& OutError)
{
    const FString NormalizedHelper = NormalizeTypeName(Helper);
    const FString NormalizedStructType = NormalizeTypeName(StructType);
    const bool bMake = NormalizedHelper == TEXT("make");
    const bool bBreak = NormalizedHelper == TEXT("break");

    if (!bMake && !bBreak)
    {
        OutError = FString::Printf(TEXT("Unsupported struct helper operation: %s"), *Helper);
        return NAME_None;
    }

    if (NormalizedStructType == TEXT("vector"))
    {
        return bMake
            ? GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, MakeVector)
            : GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, BreakVector);
    }
    if (NormalizedStructType == TEXT("rotator"))
    {
        return bMake
            ? GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, MakeRotator)
            : GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, BreakRotator);
    }
    if (NormalizedStructType == TEXT("transform"))
    {
        return bMake
            ? GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, MakeTransform)
            : GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, BreakTransform);
    }

    OutError = FString::Printf(TEXT("Unsupported native struct helper type: %s"), *StructType);
    return NAME_None;
}

bool TryReadIntegerField(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, int32& OutValue, FString& OutError)
{
    if (!Params.IsValid() || !Params->HasField(FieldName))
    {
        return false;
    }

    const TSharedPtr<FJsonValue> Value = Params->TryGetField(FieldName);
    if (!Value.IsValid() || Value->Type != EJson::Number)
    {
        OutError = FString::Printf(TEXT("'%s' must be a number"), *FieldName);
        return false;
    }

    const double NumberValue = Value->AsNumber();
    const int32 RoundedValue = FMath::RoundToInt(NumberValue);
    if (!FMath::IsNearlyEqual(NumberValue, static_cast<double>(RoundedValue)))
    {
        OutError = FString::Printf(TEXT("'%s' must be an integer"), *FieldName);
        return false;
    }

    OutValue = RoundedValue;
    return true;
}

UEdGraph* ResolveStandardMacroGraph(const FString& MacroName, FString& OutError)
{
    OutError.Reset();
    static const TCHAR* StandardMacroPaths[] = {
        TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"),
        TEXT("/Engine/EditorKismetResources/StandardMacros.StandardMacros")
    };

    for (const TCHAR* MacroPath : StandardMacroPaths)
    {
        UBlueprint* MacroBlueprint = LoadObject<UBlueprint>(nullptr, MacroPath);
        if (!MacroBlueprint)
        {
            continue;
        }

        TArray<UEdGraph*> Graphs;
        MacroBlueprint->GetAllGraphs(Graphs);
        for (UEdGraph* Graph : Graphs)
        {
            if (Graph && Graph->GetName().Equals(MacroName, ESearchCase::IgnoreCase))
            {
                return Graph;
            }
        }
    }

    OutError = FString::Printf(TEXT("Standard macro graph not found: %s"), *MacroName);
    return nullptr;
}

FName ResolveLoopMacroName(const FString& LoopType, FString& OutError)
{
    const FString NormalizedLoopType = NormalizeTypeName(LoopType.IsEmpty() ? TEXT("for_loop") : LoopType);
    if (NormalizedLoopType == TEXT("for") || NormalizedLoopType == TEXT("forloop"))
    {
        return TEXT("ForLoop");
    }

    OutError = FString::Printf(TEXT("Unsupported loop_type '%s'. Supported loop_type: for_loop"), *LoopType);
    return NAME_None;
}

bool ApplyOptionalIntegerPinDefault(
    const TSharedPtr<FJsonObject>& Params,
    const FString& FieldName,
    UEdGraphNode* Node,
    const FString& PinName,
    const UEdGraphSchema_K2* K2Schema,
    TSharedPtr<FJsonObject> AppliedDefaults,
    FString& OutError)
{
    if (!Params.IsValid() || !Params->HasField(FieldName))
    {
        return true;
    }

    int32 IntegerValue = 0;
    FString ParseError;
    if (!TryReadIntegerField(Params, FieldName, IntegerValue, ParseError))
    {
        OutError = ParseError.IsEmpty()
            ? FString::Printf(TEXT("Failed to parse integer default field: %s"), *FieldName)
            : ParseError;
        return false;
    }

    UEdGraphPin* Pin = FUnrealMCPCommonUtils::FindPin(Node, PinName, EGPD_Input);
    if (!Pin)
    {
        OutError = FString::Printf(TEXT("Loop pin not found: %s"), *PinName);
        return false;
    }

    FString AppliedValue;
    FString DefaultError;
    if (!ApplyPinDefaultValueChecked(Pin, MakeShared<FJsonValueNumber>(IntegerValue), K2Schema, AppliedValue, DefaultError))
    {
        OutError = FString::Printf(TEXT("Invalid default value for pin '%s': %s"), *PinName, *DefaultError);
        return false;
    }

    if (UK2Node* K2Node = Cast<UK2Node>(Node))
    {
        K2Node->PinDefaultValueChanged(Pin);
    }
    AppliedDefaults->SetStringField(PinName, AppliedValue);
    return true;
}

FName ResolveArrayOperationFunctionName(const FString& Operation, FString& OutError)
{
    const FString NormalizedOperation = NormalizeTypeName(Operation);
    if (NormalizedOperation == TEXT("length") || NormalizedOperation == TEXT("num") || NormalizedOperation == TEXT("count") || NormalizedOperation == TEXT("size"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_Length);
    }
    if (NormalizedOperation == TEXT("get"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_Get);
    }
    if (NormalizedOperation == TEXT("set") || NormalizedOperation == TEXT("setarrayelem"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_Set);
    }
    if (NormalizedOperation == TEXT("add"))
    {
        return GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_Add);
    }

    OutError = FString::Printf(TEXT("Unsupported array operation '%s'. Supported operations: length, get, set, add"), *Operation);
    return NAME_None;
}

bool ApplyPinTypeToArrayFunctionNode(UK2Node_CallArrayFunction* ArrayNode, const FEdGraphPinType& ElementPinType, FString& OutError)
{
    if (!ArrayNode)
    {
        OutError = TEXT("Invalid array function node");
        return false;
    }

    UEdGraphPin* TargetArrayPin = ArrayNode->GetTargetArrayPin();
    if (!TargetArrayPin)
    {
        OutError = TEXT("Array function target array pin not found");
        return false;
    }

    TargetArrayPin->PinType.PinCategory = ElementPinType.PinCategory;
    TargetArrayPin->PinType.PinSubCategory = ElementPinType.PinSubCategory;
    TargetArrayPin->PinType.PinSubCategoryObject = ElementPinType.PinSubCategoryObject;
    TargetArrayPin->PinType.ContainerType = EPinContainerType::Array;
    TargetArrayPin->PinType.bIsReference = true;

    UFunction* TargetFunction = ArrayNode->GetTargetFunction();
    if (!TargetFunction)
    {
        OutError = TEXT("Array function target function not found");
        return false;
    }

    const FString& DependentPinsMetadata = TargetFunction->GetMetaData(FBlueprintMetadata::MD_ArrayDependentParam);
    TArray<FString> DependentPinNames;
    DependentPinsMetadata.ParseIntoArray(DependentPinNames, TEXT(","), true);
    for (const FString& DependentPinName : DependentPinNames)
    {
        UEdGraphPin* DependentPin = FUnrealMCPCommonUtils::FindPin(ArrayNode, DependentPinName, EGPD_Input);
        if (!DependentPin)
        {
            DependentPin = FUnrealMCPCommonUtils::FindPin(ArrayNode, DependentPinName, EGPD_Output);
        }
        if (!DependentPin)
        {
            continue;
        }

        DependentPin->PinType.PinCategory = ElementPinType.PinCategory;
        DependentPin->PinType.PinSubCategory = ElementPinType.PinSubCategory;
        DependentPin->PinType.PinSubCategoryObject = ElementPinType.PinSubCategoryObject;
        DependentPin->PinType.ContainerType = EPinContainerType::None;
        DependentPin->PinType.bIsReference = false;
        GetDefault<UEdGraphSchema_K2>()->SetPinAutogeneratedDefaultValueBasedOnType(DependentPin);
    }

    return true;
}

bool ResolveMakeArrayInputs(
    const TSharedPtr<FJsonObject>& Params,
    const TArray<TSharedPtr<FJsonValue>>*& OutValues,
    bool& bOutHasValues,
    int32& OutInputCount,
    FString& OutError)
{
    constexpr int32 MaxInputCount = 128;

    OutValues = nullptr;
    bOutHasValues = false;
    OutInputCount = 1;
    OutError.Reset();

    if (!Params.IsValid())
    {
        OutError = TEXT("Invalid parameters");
        return false;
    }

    if (Params->HasField(TEXT("values")))
    {
        if (!Params->TryGetArrayField(TEXT("values"), OutValues))
        {
            OutError = TEXT("'values' must be an array");
            return false;
        }
        bOutHasValues = true;
        OutInputCount = OutValues ? OutValues->Num() : 0;
    }
    else if (Params->HasField(TEXT("element_defaults")))
    {
        if (!Params->TryGetArrayField(TEXT("element_defaults"), OutValues))
        {
            OutError = TEXT("'element_defaults' must be an array");
            return false;
        }
        bOutHasValues = true;
        OutInputCount = OutValues ? OutValues->Num() : 0;
    }

    if (Params->HasField(TEXT("input_count")))
    {
        int32 ParsedInputCount = 0;
        FString ParseError;
        if (!TryReadIntegerField(Params, TEXT("input_count"), ParsedInputCount, ParseError))
        {
            OutError = ParseError.IsEmpty() ? TEXT("'input_count' must be an integer") : ParseError;
            return false;
        }
        OutInputCount = ParsedInputCount;
    }

    if (OutInputCount < 0 || OutInputCount > MaxInputCount)
    {
        OutError = FString::Printf(TEXT("'input_count' must be between 0 and %d"), MaxInputCount);
        return false;
    }

    if (bOutHasValues && OutValues && OutValues->Num() > OutInputCount)
    {
        OutError = TEXT("'values' cannot contain more entries than 'input_count'");
        return false;
    }

    return true;
}

bool ApplyPinTypeToMakeArrayNode(UK2Node_MakeArray* MakeArrayNode, const FEdGraphPinType& ElementPinType, FString& OutError)
{
    if (!MakeArrayNode)
    {
        OutError = TEXT("Invalid make array node");
        return false;
    }

    UEdGraphPin* OutputPin = MakeArrayNode->GetOutputPin();
    if (!OutputPin)
    {
        OutError = TEXT("Make array output pin not found");
        return false;
    }

    OutputPin->PinType.PinCategory = ElementPinType.PinCategory;
    OutputPin->PinType.PinSubCategory = ElementPinType.PinSubCategory;
    OutputPin->PinType.PinSubCategoryObject = ElementPinType.PinSubCategoryObject;
    OutputPin->PinType.ContainerType = EPinContainerType::Array;
    OutputPin->PinType.PinValueType = FEdGraphTerminalType();
    OutputPin->PinType.bIsReference = false;

    const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
    int32 InputPinCount = 0;
    for (UEdGraphPin* Pin : MakeArrayNode->Pins)
    {
        if (!Pin || Pin->Direction != EGPD_Input || Pin->ParentPin)
        {
            continue;
        }

        Pin->PinType.PinCategory = ElementPinType.PinCategory;
        Pin->PinType.PinSubCategory = ElementPinType.PinSubCategory;
        Pin->PinType.PinSubCategoryObject = ElementPinType.PinSubCategoryObject;
        Pin->PinType.ContainerType = EPinContainerType::None;
        Pin->PinType.PinValueType = FEdGraphTerminalType();
        Pin->PinType.bIsReference = false;
        if (K2Schema)
        {
            K2Schema->SetPinAutogeneratedDefaultValueBasedOnType(Pin);
        }
        ++InputPinCount;
    }

    if (InputPinCount != MakeArrayNode->NumInputs)
    {
        OutError = FString::Printf(TEXT("Make array input pin count mismatch. expected=%d actual=%d"), MakeArrayNode->NumInputs, InputPinCount);
        return false;
    }

    return true;
}

bool ResolveSwitchIntCases(const TSharedPtr<FJsonObject>& Params, int32& OutStartIndex, int32& OutCaseCount, FString& OutError)
{
    OutStartIndex = 0;
    OutCaseCount = 2;

    int32 ParsedStartIndex = 0;
    FString ParseError;
    if (TryReadIntegerField(Params, TEXT("start_index"), ParsedStartIndex, ParseError))
    {
        OutStartIndex = ParsedStartIndex;
    }
    else if (!ParseError.IsEmpty())
    {
        OutError = ParseError;
        return false;
    }

    int32 ParsedCaseCount = 2;
    ParseError.Reset();
    if (TryReadIntegerField(Params, TEXT("case_count"), ParsedCaseCount, ParseError))
    {
        OutCaseCount = ParsedCaseCount;
    }
    else if (!ParseError.IsEmpty())
    {
        OutError = ParseError;
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* CaseValues = nullptr;
    if (Params->HasField(TEXT("case_values")) && !Params->TryGetArrayField(TEXT("case_values"), CaseValues))
    {
        OutError = TEXT("'case_values' must be an array");
        return false;
    }

    if (CaseValues)
    {
        if (CaseValues->Num() == 0)
        {
            OutError = TEXT("'case_values' must contain at least one integer case");
            return false;
        }

        TSet<int32> SeenCases;
        int32 PreviousCase = 0;
        for (int32 Index = 0; Index < CaseValues->Num(); ++Index)
        {
            const TSharedPtr<FJsonValue>& CaseValue = (*CaseValues)[Index];
            if (!CaseValue.IsValid() || CaseValue->Type != EJson::Number)
            {
                OutError = TEXT("'case_values' entries must be integers");
                return false;
            }

            const double NumberValue = CaseValue->AsNumber();
            const int32 IntegerValue = FMath::RoundToInt(NumberValue);
            if (!FMath::IsNearlyEqual(NumberValue, static_cast<double>(IntegerValue)))
            {
                OutError = TEXT("'case_values' entries must be integers");
                return false;
            }
            if (SeenCases.Contains(IntegerValue))
            {
                OutError = FString::Printf(TEXT("Duplicate switch int case: %d"), IntegerValue);
                return false;
            }
            SeenCases.Add(IntegerValue);

            if (Index == 0)
            {
                OutStartIndex = IntegerValue;
            }
            else if (IntegerValue != PreviousCase + 1)
            {
                OutError = TEXT("Sparse switch int cases are not supported; use ascending contiguous case values");
                return false;
            }

            PreviousCase = IntegerValue;
        }

        OutCaseCount = CaseValues->Num();
    }

    if (OutCaseCount <= 0)
    {
        OutError = TEXT("'case_count' must be greater than zero");
        return false;
    }
    if (OutCaseCount > 128)
    {
        OutError = TEXT("'case_count' must be 128 or less");
        return false;
    }

    return true;
}

FString NormalizeGraphType(const FString& GraphType)
{
    if (GraphType.IsEmpty())
    {
        return FString();
    }

    FString Normalized = GraphType.ToLower();
    Normalized.ReplaceInline(TEXT(" "), TEXT(""));
    Normalized.ReplaceInline(TEXT("-"), TEXT("_"));

    if (Normalized == TEXT("eventgraph") || Normalized == TEXT("event_graph") || Normalized == TEXT("ubergraph") || Normalized == TEXT("uber_graph"))
    {
        return TEXT("event");
    }
    if (Normalized == TEXT("functiongraph") || Normalized == TEXT("function_graph"))
    {
        return TEXT("function");
    }
    if (Normalized == TEXT("macrograph") || Normalized == TEXT("macro_graph"))
    {
        return TEXT("macro");
    }

    return Normalized;
}

bool BlueprintGraphArrayContains(const TArray<TObjectPtr<UEdGraph>>& GraphArray, const UEdGraph* Graph)
{
    for (const TObjectPtr<UEdGraph>& CandidateGraph : GraphArray)
    {
        if (CandidateGraph.Get() == Graph)
        {
            return true;
        }
    }

    return false;
}

FString GetBlueprintGraphType(const UBlueprint* Blueprint, const UEdGraph* Graph)
{
    if (!Blueprint || !Graph)
    {
        return TEXT("unknown");
    }

    if (BlueprintGraphArrayContains(Blueprint->FunctionGraphs, Graph))
    {
        return TEXT("function");
    }
    if (BlueprintGraphArrayContains(Blueprint->MacroGraphs, Graph))
    {
        return TEXT("macro");
    }
    if (BlueprintGraphArrayContains(Blueprint->UbergraphPages, Graph) || BlueprintGraphArrayContains(Blueprint->EventGraphs, Graph))
    {
        return TEXT("event");
    }
    if (BlueprintGraphArrayContains(Blueprint->DelegateSignatureGraphs, Graph))
    {
        return TEXT("delegate");
    }

    return TEXT("unknown");
}

bool GraphMatchesSelector(const UBlueprint* Blueprint, const UEdGraph* Graph, const FString& GraphId, const FString& GraphName, const FString& GraphType)
{
    if (!Graph)
    {
        return false;
    }

    if (!GraphId.IsEmpty())
    {
        FGuid ParsedGraphId;
        if (FGuid::Parse(GraphId, ParsedGraphId))
        {
            if (Graph->GraphGuid != ParsedGraphId)
            {
                return false;
            }
        }
        else if (Graph->GraphGuid.ToString() != GraphId)
        {
            return false;
        }
    }

    if (!GraphName.IsEmpty() && !Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
    {
        return false;
    }

    const FString NormalizedType = NormalizeGraphType(GraphType);
    if (!NormalizedType.IsEmpty() && NormalizedType != TEXT("any") && GetBlueprintGraphType(Blueprint, Graph) != NormalizedType)
    {
        return false;
    }

    return true;
}

UEdGraph* FindBlueprintGraph(UBlueprint* Blueprint, const FString& GraphId, const FString& GraphName, const FString& GraphType)
{
    if (!Blueprint)
    {
        return nullptr;
    }

    TArray<UEdGraph*> Graphs;
    Blueprint->GetAllGraphs(Graphs);
    for (UEdGraph* Graph : Graphs)
    {
        if (GraphMatchesSelector(Blueprint, Graph, GraphId, GraphName, GraphType))
        {
            return Graph;
        }
    }

    return nullptr;
}

TSharedPtr<FJsonObject> GraphToJson(const UBlueprint* Blueprint, const UEdGraph* Graph)
{
    TSharedPtr<FJsonObject> GraphObject = MakeShared<FJsonObject>();
    if (!Graph)
    {
        return GraphObject;
    }

    GraphObject->SetStringField(TEXT("graph_id"), Graph->GraphGuid.ToString());
    GraphObject->SetStringField(TEXT("graph_name"), Graph->GetName());
    GraphObject->SetStringField(TEXT("graph_type"), GetBlueprintGraphType(Blueprint, Graph));
    GraphObject->SetStringField(TEXT("schema"), Graph->GetSchema() ? Graph->GetSchema()->GetClass()->GetName() : FString());
    GraphObject->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
    return GraphObject;
}

FMulticastDelegateProperty* FindBlueprintMulticastDelegateProperty(UBlueprint* Blueprint, const FName& DispatcherName)
{
    if (!Blueprint || DispatcherName.IsNone())
    {
        return nullptr;
    }

    TArray<UClass*> CandidateClasses;
    if (Blueprint->SkeletonGeneratedClass)
    {
        CandidateClasses.Add(Blueprint->SkeletonGeneratedClass);
    }
    if (Blueprint->GeneratedClass)
    {
        CandidateClasses.Add(Blueprint->GeneratedClass);
    }
    if (Blueprint->ParentClass)
    {
        CandidateClasses.Add(Blueprint->ParentClass);
    }

    for (UClass* CandidateClass : CandidateClasses)
    {
        if (!CandidateClass)
        {
            continue;
        }

        if (FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(CandidateClass, DispatcherName))
        {
            return DelegateProperty;
        }
    }

    return nullptr;
}

bool IsSelfContextForDelegateProperty(const UBlueprint* Blueprint, const FMulticastDelegateProperty* DelegateProperty)
{
    if (!Blueprint || !DelegateProperty)
    {
        return false;
    }

    const UClass* VariableSourceClass = DelegateProperty->GetOwnerClass();
    return VariableSourceClass == nullptr ||
        (Blueprint->SkeletonGeneratedClass && Blueprint->SkeletonGeneratedClass->IsChildOf(VariableSourceClass));
}

UEdGraph* ResolveBlueprintGraphForNodeCommand(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params, FString& OutError);
void AddGraphField(TSharedPtr<FJsonObject> ResultObj, const UBlueprint* Blueprint, const UEdGraph* Graph);

struct FEventDispatcherNodeRequest
{
    FString BlueprintName;
    FString DispatcherName;
    FVector2D NodePosition = FVector2D(0.0f, 0.0f);
    UBlueprint* Blueprint = nullptr;
    UEdGraph* TargetGraph = nullptr;
    FMulticastDelegateProperty* DelegateProperty = nullptr;
};

bool ResolveEventDispatcherNodeRequest(
    const TSharedPtr<FJsonObject>& Params,
    const TCHAR* ActionName,
    FEventDispatcherNodeRequest& OutRequest,
    FString& OutError)
{
    if (!Params->TryGetStringField(TEXT("blueprint_name"), OutRequest.BlueprintName))
    {
        OutError = TEXT("Missing 'blueprint_name' parameter");
        return false;
    }

    if (!Params->TryGetStringField(TEXT("dispatcher_name"), OutRequest.DispatcherName) &&
        !Params->TryGetStringField(TEXT("event_dispatcher_name"), OutRequest.DispatcherName) &&
        !Params->TryGetStringField(TEXT("delegate_name"), OutRequest.DispatcherName))
    {
        OutError = TEXT("Missing 'dispatcher_name' parameter");
        return false;
    }

    OutRequest.DispatcherName.TrimStartAndEndInline();
    if (OutRequest.DispatcherName.IsEmpty())
    {
        OutError = TEXT("'dispatcher_name' cannot be empty");
        return false;
    }

    if (Params->HasField(TEXT("node_position")))
    {
        OutRequest.NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    OutRequest.Blueprint = FUnrealMCPCommonUtils::FindBlueprint(OutRequest.BlueprintName);
    if (!OutRequest.Blueprint)
    {
        OutError = FString::Printf(TEXT("Blueprint not found: %s"), *OutRequest.BlueprintName);
        return false;
    }

    FString GraphError;
    OutRequest.TargetGraph = ResolveBlueprintGraphForNodeCommand(OutRequest.Blueprint, Params, GraphError);
    if (!OutRequest.TargetGraph)
    {
        OutError = GraphError;
        return false;
    }

    OutRequest.DelegateProperty = FindBlueprintMulticastDelegateProperty(OutRequest.Blueprint, FName(*OutRequest.DispatcherName));
    if (!OutRequest.DelegateProperty)
    {
        OutError = FString::Printf(TEXT("Event dispatcher property not found: %s. Compile or recreate the dispatcher before adding a %s node."), *OutRequest.DispatcherName, ActionName);
        return false;
    }
    if (!OutRequest.DelegateProperty->HasAllPropertyFlags(CPF_BlueprintAssignable))
    {
        OutError = FString::Printf(TEXT("Event dispatcher is not BlueprintAssignable: %s"), *OutRequest.DispatcherName);
        return false;
    }
    if (!OutRequest.DelegateProperty->SignatureFunction)
    {
        OutError = FString::Printf(TEXT("Event dispatcher signature is not available: %s"), *OutRequest.DispatcherName);
        return false;
    }

    return true;
}

template <typename NodeType>
TSharedPtr<FJsonObject> CreateEventDispatcherLifecycleNode(
    const FEventDispatcherNodeRequest& Request,
    const TCHAR* NodeKind,
    bool bAllocatePinsBeforePostPlaced,
    bool bReconstructAfterPostPlaced,
    bool bMarkStructurallyModified)
{
    NodeType* LifecycleNode = NewObject<NodeType>(Request.TargetGraph);
    if (!LifecycleNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to create Event Dispatcher %s node"), NodeKind));
    }

    LifecycleNode->SetFromProperty(
        Request.DelegateProperty,
        IsSelfContextForDelegateProperty(Request.Blueprint, Request.DelegateProperty),
        Request.DelegateProperty->GetOwnerClass());
    if (!LifecycleNode->IsCompatibleWithGraph(Request.TargetGraph))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Event Dispatcher %s nodes are not supported in this graph"), NodeKind));
    }

    LifecycleNode->NodePosX = Request.NodePosition.X;
    LifecycleNode->NodePosY = Request.NodePosition.Y;
    Request.TargetGraph->AddNode(LifecycleNode, true);
    LifecycleNode->CreateNewGuid();

    if (bAllocatePinsBeforePostPlaced)
    {
        LifecycleNode->AllocateDefaultPins();
        LifecycleNode->PostPlacedNewNode();
    }
    else
    {
        LifecycleNode->PostPlacedNewNode();
        LifecycleNode->AllocateDefaultPins();
    }

    if (bReconstructAfterPostPlaced)
    {
        LifecycleNode->ReconstructNode();
    }

    if (bMarkStructurallyModified)
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Request.Blueprint);
    }
    else
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(Request.Blueprint);
    }

    TSharedPtr<FJsonObject> ResultObj = NodeToJson(LifecycleNode, true);
    ResultObj->SetStringField(TEXT("dispatcher_name"), Request.DispatcherName);
    ResultObj->SetStringField(TEXT("node_kind"), NodeKind);
    ResultObj->SetStringField(TEXT("signature_function"), Request.DelegateProperty->SignatureFunction->GetPathName());
    AddGraphField(ResultObj, Request.Blueprint, Request.TargetGraph);
    return ResultObj;
}

UEdGraph* CreateBlueprintFunctionGraph(UBlueprint* Blueprint, const FString& RequestedGraphName, FString& OutError)
{
    if (!Blueprint)
    {
        OutError = TEXT("Invalid blueprint");
        return nullptr;
    }
    if (RequestedGraphName.IsEmpty())
    {
        OutError = TEXT("Missing 'graph_name' parameter for function graph creation");
        return nullptr;
    }

    const FName NewGraphName = FBlueprintEditorUtils::FindUniqueKismetName(Blueprint, RequestedGraphName);
    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
        Blueprint,
        NewGraphName,
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());

    if (!NewGraph)
    {
        OutError = FString::Printf(TEXT("Failed to create function graph: %s"), *RequestedGraphName);
        return nullptr;
    }

    FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewGraph, true, nullptr);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    return NewGraph;
}

UEdGraph* ResolveBlueprintGraph(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params, bool bCreateIfMissing, bool& bOutCreated, FString& OutError)
{
    bOutCreated = false;
    OutError.Reset();

    if (!Blueprint)
    {
        OutError = TEXT("Invalid blueprint");
        return nullptr;
    }

    FString GraphId;
    Params->TryGetStringField(TEXT("graph_id"), GraphId);

    FString GraphName;
    Params->TryGetStringField(TEXT("graph_name"), GraphName);

    FString GraphType;
    Params->TryGetStringField(TEXT("graph_type"), GraphType);
    const FString NormalizedType = NormalizeGraphType(GraphType);

    const bool bHasGraphSelector = !GraphId.IsEmpty() || !GraphName.IsEmpty() || !NormalizedType.IsEmpty();
    if (!bHasGraphSelector)
    {
        UEdGraph* EventGraph = FUnrealMCPCommonUtils::FindOrCreateEventGraph(Blueprint);
        if (!EventGraph)
        {
            OutError = TEXT("Failed to get event graph");
        }
        return EventGraph;
    }

    if (UEdGraph* ExistingGraph = FindBlueprintGraph(Blueprint, GraphId, GraphName, GraphType))
    {
        return ExistingGraph;
    }

    if (!bCreateIfMissing)
    {
        OutError = FString::Printf(TEXT("Blueprint graph not found. graph_name='%s', graph_id='%s', graph_type='%s'"), *GraphName, *GraphId, *GraphType);
        return nullptr;
    }

    if (!GraphId.IsEmpty())
    {
        OutError = TEXT("Cannot create a graph with an explicit graph_id");
        return nullptr;
    }

    if (NormalizedType == TEXT("function"))
    {
        UEdGraph* CreatedGraph = CreateBlueprintFunctionGraph(Blueprint, GraphName, OutError);
        bOutCreated = CreatedGraph != nullptr;
        return CreatedGraph;
    }

    if (NormalizedType == TEXT("event") && GraphName.IsEmpty())
    {
        UEdGraph* EventGraph = FUnrealMCPCommonUtils::FindOrCreateEventGraph(Blueprint);
        bOutCreated = EventGraph != nullptr;
        if (!EventGraph)
        {
            OutError = TEXT("Failed to get event graph");
        }
        return EventGraph;
    }

    OutError = FString::Printf(TEXT("Creating graph_type '%s' is not supported by this command"), *GraphType);
    return nullptr;
}

UEdGraph* ResolveBlueprintGraphForNodeCommand(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
    bool bCreateIfMissing = false;
    if (Params->HasField(TEXT("create_graph_if_missing")))
    {
        bCreateIfMissing = Params->GetBoolField(TEXT("create_graph_if_missing"));
    }

    bool bCreated = false;
    return ResolveBlueprintGraph(Blueprint, Params, bCreateIfMissing, bCreated, OutError);
}

void AddGraphField(TSharedPtr<FJsonObject> ResultObj, const UBlueprint* Blueprint, const UEdGraph* Graph)
{
    if (ResultObj.IsValid() && Graph)
    {
        ResultObj->SetObjectField(TEXT("graph"), GraphToJson(Blueprint, Graph));
    }
}

template <typename NodeType>
NodeType* AddK2NodeToGraph(UEdGraph* Graph, const FVector2D& NodePosition)
{
    if (!Graph)
    {
        return nullptr;
    }

    NodeType* Node = NewObject<NodeType>(Graph);
    if (!Node)
    {
        return nullptr;
    }

    Node->NodePosX = NodePosition.X;
    Node->NodePosY = NodePosition.Y;
    Graph->AddNode(Node, true);
    Node->CreateNewGuid();
    Node->PostPlacedNewNode();
    Node->AllocateDefaultPins();
    Node->ReconstructNode();
    return Node;
}

FString GetConnectionResponseName(ECanCreateConnectionResponse Response)
{
    switch (Response)
    {
    case CONNECT_RESPONSE_MAKE:
        return TEXT("make");
    case CONNECT_RESPONSE_DISALLOW:
        return TEXT("disallow");
    case CONNECT_RESPONSE_BREAK_OTHERS_A:
        return TEXT("break_others_a");
    case CONNECT_RESPONSE_BREAK_OTHERS_B:
        return TEXT("break_others_b");
    case CONNECT_RESPONSE_BREAK_OTHERS_AB:
        return TEXT("break_others_ab");
    case CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE:
        return TEXT("make_with_conversion_node");
    case CONNECT_RESPONSE_MAKE_WITH_PROMOTION:
        return TEXT("make_with_promotion");
    default:
        return TEXT("unknown");
    }
}

TSharedPtr<FJsonObject> CreateBlueprintFunctionLibraryNode(
    const TSharedPtr<FJsonObject>& Params,
    UClass* FunctionClass,
    const FName& FunctionName,
    const FString& NodeKind)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    if (!FunctionClass)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Function class not found for %s node"), *NodeKind));
    }

    UFunction* Function = FunctionClass->FindFunctionByName(FunctionName);
    if (!Function)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("%s function not found: %s"), *NodeKind, *FunctionName.ToString()));
    }

    UK2Node_CallFunction* FunctionNode = FUnrealMCPCommonUtils::CreateFunctionCallNode(TargetGraph, Function, NodePosition);
    if (!FunctionNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to create %s node"), *NodeKind));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    TSharedPtr<FJsonObject> ResultObj = NodeToJson(FunctionNode, true);
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}
}

FUnrealMCPBlueprintNodeCommands::FUnrealMCPBlueprintNodeCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("connect_blueprint_nodes"))
    {
        return HandleConnectBlueprintNodes(Params);
    }
    else if (CommandType == TEXT("resolve_blueprint"))
    {
        return HandleResolveBlueprint(Params);
    }
    else if (CommandType == TEXT("list_blueprint_nodes"))
    {
        return HandleListBlueprintNodes(Params);
    }
    else if (CommandType == TEXT("list_blueprint_graphs"))
    {
        return HandleListBlueprintGraphs(Params);
    }
    else if (CommandType == TEXT("resolve_blueprint_graph"))
    {
        return HandleResolveBlueprintGraph(Params);
    }
    else if (CommandType == TEXT("add_blueprint_get_self_component_reference"))
    {
        return HandleAddBlueprintGetSelfComponentReference(Params);
    }
    else if (CommandType == TEXT("add_blueprint_event_node"))
    {
        return HandleAddBlueprintEvent(Params);
    }
    else if (CommandType == TEXT("add_blueprint_function_node"))
    {
        return HandleAddBlueprintFunctionCall(Params);
    }
    else if (CommandType == TEXT("add_blueprint_call_function_node"))
    {
        return HandleAddBlueprintCallFunctionNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_variable"))
    {
        return HandleAddBlueprintVariable(Params);
    }
    else if (CommandType == TEXT("add_blueprint_event_dispatcher"))
    {
        return HandleAddBlueprintEventDispatcher(Params);
    }
    else if (CommandType == TEXT("add_blueprint_event_dispatcher_call_node"))
    {
        return HandleAddBlueprintEventDispatcherCallNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_custom_event_node"))
    {
        return HandleAddBlueprintCustomEventNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_event_dispatcher_bind_node"))
    {
        return HandleAddBlueprintEventDispatcherBindNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_event_dispatcher_unbind_node"))
    {
        return HandleAddBlueprintEventDispatcherUnbindNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_event_dispatcher_clear_node"))
    {
        return HandleAddBlueprintEventDispatcherClearNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_event_dispatcher_assign_node"))
    {
        return HandleAddBlueprintEventDispatcherAssignNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_function_parameter"))
    {
        return HandleAddBlueprintFunctionParameter(Params);
    }
    else if (CommandType == TEXT("add_blueprint_local_variable"))
    {
        return HandleAddBlueprintLocalVariable(Params);
    }
    else if (CommandType == TEXT("add_blueprint_branch_node"))
    {
        return HandleAddBlueprintBranchNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_sequence_node"))
    {
        return HandleAddBlueprintSequenceNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_return_node"))
    {
        return HandleAddBlueprintReturnNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_dynamic_cast_node"))
    {
        return HandleAddBlueprintDynamicCastNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_loop_node"))
    {
        return HandleAddBlueprintLoopNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_array_function_node"))
    {
        return HandleAddBlueprintArrayFunctionNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_make_array_node"))
    {
        return HandleAddBlueprintMakeArrayNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_input_action_node"))
    {
        return HandleAddBlueprintInputActionNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_input_axis_event_node"))
    {
        return HandleAddBlueprintInputAxisEventNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_enhanced_input_action_node"))
    {
        return HandleAddBlueprintEnhancedInputActionNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_variable_get_node"))
    {
        return HandleAddBlueprintVariableGetNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_variable_set_node"))
    {
        return HandleAddBlueprintVariableSetNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_math_node"))
    {
        return HandleAddBlueprintMathNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_compare_node"))
    {
        return HandleAddBlueprintCompareNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_boolean_node"))
    {
        return HandleAddBlueprintBooleanNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_select_node"))
    {
        return HandleAddBlueprintSelectNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_literal_node"))
    {
        return HandleAddBlueprintLiteralNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_enum_literal_node"))
    {
        return HandleAddBlueprintEnumLiteralNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_is_valid_node"))
    {
        return HandleAddBlueprintIsValidNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_make_struct_node"))
    {
        return HandleAddBlueprintMakeStructNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_break_struct_node"))
    {
        return HandleAddBlueprintBreakStructNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_switch_int_node"))
    {
        return HandleAddBlueprintSwitchIntNode(Params);
    }
    else if (CommandType == TEXT("add_blueprint_switch_enum_node"))
    {
        return HandleAddBlueprintSwitchEnumNode(Params);
    }
    else if (CommandType == TEXT("set_blueprint_pin_default"))
    {
        return HandleSetBlueprintPinDefault(Params);
    }
    else if (CommandType == TEXT("add_blueprint_self_reference"))
    {
        return HandleAddBlueprintSelfReference(Params);
    }
    else if (CommandType == TEXT("find_blueprint_nodes"))
    {
        return HandleFindBlueprintNodes(Params);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown blueprint node command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleConnectBlueprintNodes(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString SourceNodeId;
    if (!Params->TryGetStringField(TEXT("source_node_id"), SourceNodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'source_node_id' parameter"));
    }

    FString TargetNodeId;
    if (!Params->TryGetStringField(TEXT("target_node_id"), TargetNodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'target_node_id' parameter"));
    }

    FString SourcePinName;
    if (!Params->TryGetStringField(TEXT("source_pin"), SourcePinName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'source_pin' parameter"));
    }

    FString TargetPinName;
    if (!Params->TryGetStringField(TEXT("target_pin"), TargetPinName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'target_pin' parameter"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    // Find the nodes
    UEdGraphNode* SourceNode = FindNodeById(TargetGraph, SourceNodeId);
    UEdGraphNode* TargetNode = FindNodeById(TargetGraph, TargetNodeId);

    if (!SourceNode || !TargetNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Source or target node not found"));
    }

    UEdGraphPin* SourcePin = FUnrealMCPCommonUtils::FindPin(SourceNode, SourcePinName, EGPD_Output);
    if (!SourcePin)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Source output pin not found: %s"), *SourcePinName));
    }

    UEdGraphPin* TargetPin = FUnrealMCPCommonUtils::FindPin(TargetNode, TargetPinName, EGPD_Input);
    if (!TargetPin)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Target input pin not found: %s"), *TargetPinName));
    }

    const UEdGraphSchema_K2* K2Schema = Cast<const UEdGraphSchema_K2>(TargetGraph->GetSchema());
    if (!K2Schema)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get K2 schema"));
    }

    const FPinConnectionResponse ConnectionResponse = K2Schema->CanCreateConnection(SourcePin, TargetPin);

    bool bAllowPinLinkReplacement = false;
    if (Params->HasField(TEXT("allow_pin_link_replacement")))
    {
        bAllowPinLinkReplacement = Params->GetBoolField(TEXT("allow_pin_link_replacement"));
    }

    if (!ConnectionResponse.CanSafeConnect() && (!bAllowPinLinkReplacement || ConnectionResponse.Response == CONNECT_RESPONSE_DISALLOW))
    {
        TSharedPtr<FJsonObject> ErrorObj = FUnrealMCPCommonUtils::CreateErrorResponse(ConnectionResponse.Message.ToString());
        ErrorObj->SetStringField(TEXT("schema_response"), GetConnectionResponseName(ConnectionResponse.Response));
        ErrorObj->SetStringField(TEXT("schema_message"), ConnectionResponse.Message.ToString());
        ErrorObj->SetObjectField(TEXT("source_pin"), PinToJson(SourcePin));
        ErrorObj->SetObjectField(TEXT("target_pin"), PinToJson(TargetPin));
        return ErrorObj;
    }

    if (!K2Schema->TryCreateConnection(SourcePin, TargetPin))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("K2 schema failed to create connection"));
    }

    // Mark the blueprint as modified
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("source_node_id"), SourceNodeId);
    ResultObj->SetStringField(TEXT("target_node_id"), TargetNodeId);
    ResultObj->SetStringField(TEXT("source_pin"), SourcePinName);
    ResultObj->SetStringField(TEXT("target_pin"), TargetPinName);
    ResultObj->SetBoolField(TEXT("connected"), true);
    ResultObj->SetStringField(TEXT("schema_response"), GetConnectionResponseName(ConnectionResponse.Response));
    ResultObj->SetStringField(TEXT("schema_message"), ConnectionResponse.Message.ToString());
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintGetSelfComponentReference(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ComponentName;
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'component_name' parameter"));
    }

    // Get position parameters (optional)
    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    // We'll skip component verification since the GetAllNodes API may have changed in UE5.5

    // Create the variable get node directly
    UK2Node_VariableGet* GetComponentNode = NewObject<UK2Node_VariableGet>(TargetGraph);
    if (!GetComponentNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create get component node"));
    }

    // Set up the variable reference properly for UE5.5
    FMemberReference& VarRef = GetComponentNode->VariableReference;
    VarRef.SetSelfMember(FName(*ComponentName));

    // Set node position
    GetComponentNode->NodePosX = NodePosition.X;
    GetComponentNode->NodePosY = NodePosition.Y;

    // Add to graph
    TargetGraph->AddNode(GetComponentNode);
    GetComponentNode->CreateNewGuid();
    GetComponentNode->PostPlacedNewNode();
    GetComponentNode->AllocateDefaultPins();

    // Explicitly reconstruct node for UE5.5
    GetComponentNode->ReconstructNode();

    // Mark the blueprint as modified
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("node_id"), GetComponentNode->NodeGuid.ToString());
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintEvent(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString EventName;
    if (!Params->TryGetStringField(TEXT("event_name"), EventName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'event_name' parameter"));
    }

    // Get position parameters (optional)
    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    // Create the event node
    UK2Node_Event* EventNode = FUnrealMCPCommonUtils::CreateEventNode(TargetGraph, EventName, NodePosition);
    if (!EventNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create event node"));
    }

    // Mark the blueprint as modified
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("node_id"), EventNode->NodeGuid.ToString());
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintFunctionCall(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString FunctionName;
    if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'function_name' parameter"));
    }

    // Get position parameters (optional)
    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    // Check for target parameter (optional)
    FString Target;
    Params->TryGetStringField(TEXT("target"), Target);

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    // Find the function
    UFunction* Function = nullptr;
    UK2Node_CallFunction* FunctionNode = nullptr;

    // Add extensive logging for debugging
    UE_LOG(LogTemp, Display, TEXT("Looking for function '%s' in target '%s'"),
           *FunctionName, Target.IsEmpty() ? TEXT("Blueprint") : *Target);

    // Check if we have a target class specified
    if (!Target.IsEmpty())
    {
        // Try to find the target class
        UClass* TargetClass = nullptr;

        // First try without a prefix
        TargetClass = FindFirstObject<UClass>(*Target);
        UE_LOG(LogTemp, Display, TEXT("Tried to find class '%s': %s"),
               *Target, TargetClass ? TEXT("Found") : TEXT("Not found"));

        // If not found, try with U prefix (common convention for UE classes)
        if (!TargetClass && !Target.StartsWith(TEXT("U")))
        {
            FString TargetWithPrefix = FString(TEXT("U")) + Target;
            TargetClass = FindFirstObject<UClass>(*TargetWithPrefix);
            UE_LOG(LogTemp, Display, TEXT("Tried to find class '%s': %s"),
                   *TargetWithPrefix, TargetClass ? TEXT("Found") : TEXT("Not found"));
        }

        // If still not found, try with common component names
        if (!TargetClass)
        {
            // Try some common component class names
            TArray<FString> PossibleClassNames;
            PossibleClassNames.Add(FString(TEXT("U")) + Target + TEXT("Component"));
            PossibleClassNames.Add(Target + TEXT("Component"));

            for (const FString& ClassName : PossibleClassNames)
            {
                TargetClass = FindFirstObject<UClass>(*ClassName);
                if (TargetClass)
                {
                    UE_LOG(LogTemp, Display, TEXT("Found class using alternative name '%s'"), *ClassName);
                    break;
                }
            }
        }

        // Special case handling for common classes like UGameplayStatics
        if (!TargetClass && Target == TEXT("UGameplayStatics"))
        {
            // For UGameplayStatics, use a direct reference to known class
            TargetClass = FindFirstObject<UClass>(TEXT("UGameplayStatics"));
            if (!TargetClass)
            {
                // Try loading it from its known package
                TargetClass = LoadObject<UClass>(nullptr, TEXT("/Script/Engine.GameplayStatics"));
                UE_LOG(LogTemp, Display, TEXT("Explicitly loading GameplayStatics: %s"),
                       TargetClass ? TEXT("Success") : TEXT("Failed"));
            }
        }

        // If we found a target class, look for the function there
        if (TargetClass)
        {
            UE_LOG(LogTemp, Display, TEXT("Looking for function '%s' in class '%s'"),
                   *FunctionName, *TargetClass->GetName());

            // First try exact name
            Function = TargetClass->FindFunctionByName(*FunctionName);

            // If not found, try class hierarchy
            UClass* CurrentClass = TargetClass;
            while (!Function && CurrentClass)
            {
                UE_LOG(LogTemp, Display, TEXT("Searching in class: %s"), *CurrentClass->GetName());

                // Try exact match
                Function = CurrentClass->FindFunctionByName(*FunctionName);

                // Try case-insensitive match
                if (!Function)
                {
                    for (TFieldIterator<UFunction> FuncIt(CurrentClass); FuncIt; ++FuncIt)
                    {
                        UFunction* AvailableFunc = *FuncIt;
                        UE_LOG(LogTemp, Display, TEXT("  - Available function: %s"), *AvailableFunc->GetName());

                        if (AvailableFunc->GetName().Equals(FunctionName, ESearchCase::IgnoreCase))
                        {
                            UE_LOG(LogTemp, Display, TEXT("  - Found case-insensitive match: %s"), *AvailableFunc->GetName());
                            Function = AvailableFunc;
                            break;
                        }
                    }
                }

                // Move to parent class
                CurrentClass = CurrentClass->GetSuperClass();
            }

            // Special handling for known functions
            if (!Function)
            {
                if (TargetClass->GetName() == TEXT("GameplayStatics") &&
                    (FunctionName == TEXT("GetActorOfClass") || FunctionName.Equals(TEXT("GetActorOfClass"), ESearchCase::IgnoreCase)))
                {
                    UE_LOG(LogTemp, Display, TEXT("Using special case handling for GameplayStatics::GetActorOfClass"));

                    // Create the function node directly
                    FunctionNode = NewObject<UK2Node_CallFunction>(TargetGraph);
                    if (FunctionNode)
                    {
                        // Direct setup for known function
                        FunctionNode->FunctionReference.SetExternalMember(
                            FName(TEXT("GetActorOfClass")),
                            TargetClass
                        );

                        FunctionNode->NodePosX = NodePosition.X;
                        FunctionNode->NodePosY = NodePosition.Y;
                        TargetGraph->AddNode(FunctionNode);
                        FunctionNode->CreateNewGuid();
                        FunctionNode->PostPlacedNewNode();
                        FunctionNode->AllocateDefaultPins();

                        UE_LOG(LogTemp, Display, TEXT("Created GetActorOfClass node directly"));

                        // List all pins
                        for (UEdGraphPin* Pin : FunctionNode->Pins)
                        {
                            UE_LOG(LogTemp, Display, TEXT("  - Pin: %s, Direction: %d, Category: %s"),
                                   *Pin->PinName.ToString(), (int32)Pin->Direction, *Pin->PinType.PinCategory.ToString());
                        }
                    }
                }
            }
        }
    }

    // If we still haven't found the function, try in the blueprint's class
    if (!Function && !FunctionNode)
    {
        UE_LOG(LogTemp, Display, TEXT("Trying to find function in blueprint class"));
        Function = Blueprint->GeneratedClass->FindFunctionByName(*FunctionName);
    }

    // Create the function call node if we found the function
    if (Function && !FunctionNode)
    {
        FunctionNode = FUnrealMCPCommonUtils::CreateFunctionCallNode(TargetGraph, Function, NodePosition);
    }

    if (!FunctionNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Function not found: %s in target %s"), *FunctionName, Target.IsEmpty() ? TEXT("Blueprint") : *Target));
    }

    // Set parameters if provided
    if (Params->HasField(TEXT("params")))
    {
        const TSharedPtr<FJsonObject>* ParamsObj;
        if (Params->TryGetObjectField(TEXT("params"), ParamsObj))
        {
            // Process parameters
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Param : (*ParamsObj)->Values)
            {
                const FString& ParamName = Param.Key;
                const TSharedPtr<FJsonValue>& ParamValue = Param.Value;

                // Find the parameter pin
                UEdGraphPin* ParamPin = FUnrealMCPCommonUtils::FindPin(FunctionNode, ParamName, EGPD_Input);
                if (ParamPin)
                {
                    UE_LOG(LogTemp, Display, TEXT("Found parameter pin '%s' of category '%s'"),
                           *ParamName, *ParamPin->PinType.PinCategory.ToString());
                    UE_LOG(LogTemp, Display, TEXT("  Current default value: '%s'"), *ParamPin->DefaultValue);
                    if (ParamPin->PinType.PinSubCategoryObject.IsValid())
                    {
                        UE_LOG(LogTemp, Display, TEXT("  Pin subcategory: '%s'"),
                               *ParamPin->PinType.PinSubCategoryObject->GetName());
                    }

                    // Set parameter based on type
                    if (ParamValue->Type == EJson::String)
                    {
                        FString StringVal = ParamValue->AsString();
                        UE_LOG(LogTemp, Display, TEXT("  Setting string parameter '%s' to: '%s'"),
                               *ParamName, *StringVal);

                        // Handle class reference parameters (e.g., ActorClass in GetActorOfClass)
                        if (ParamPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class)
                        {
                            // For class references, we require the exact class name with proper prefix
                            // - Actor classes must start with 'A' (e.g., ACameraActor)
                            // - Non-actor classes must start with 'U' (e.g., UObject)
                            const FString& ClassName = StringVal;

                            // TODO: This likely won't work in UE5.5+, so don't rely on it.
                            UClass* Class = FindFirstObject<UClass>(*ClassName);

                            if (!Class)
                            {
                                Class = LoadObject<UClass>(nullptr, *ClassName);
                                UE_LOG(LogUnrealMCP, Display, TEXT("FindObject<UClass> failed. Assuming soft path  path: %s"), *ClassName);
                            }

                            // If not found, try with Engine module path
                            if (!Class)
                            {
                                FString EngineClassName = FString::Printf(TEXT("/Script/Engine.%s"), *ClassName);
                                Class = LoadObject<UClass>(nullptr, *EngineClassName);
                                UE_LOG(LogUnrealMCP, Display, TEXT("Trying Engine module path: %s"), *EngineClassName);
                            }

                            if (!Class)
                            {
                                UE_LOG(LogUnrealMCP, Error, TEXT("Failed to find class '%s'. Make sure to use the exact class name with proper prefix (A for actors, U for non-actors)"), *ClassName);
                                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to find class '%s'"), *ClassName));
                            }

                            const UEdGraphSchema_K2* K2Schema = Cast<const UEdGraphSchema_K2>(TargetGraph->GetSchema());
                            if (!K2Schema)
                            {
                                UE_LOG(LogUnrealMCP, Error, TEXT("Failed to get K2Schema"));
                                return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get K2Schema"));
                            }

                            K2Schema->TrySetDefaultObject(*ParamPin, Class);
                            if (ParamPin->DefaultObject != Class)
                            {
                                UE_LOG(LogUnrealMCP, Error, TEXT("Failed to set class reference for pin '%s' to '%s'"), *ParamPin->PinName.ToString(), *ClassName);
                                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to set class reference for pin '%s'"), *ParamPin->PinName.ToString()));
                            }

                            UE_LOG(LogUnrealMCP, Log, TEXT("Successfully set class reference for pin '%s' to '%s'"), *ParamPin->PinName.ToString(), *ClassName);
                            continue;
                        }
                        else if (ParamPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int)
                        {
                            // Ensure we're using an integer value (no decimal)
                            int32 IntValue = FMath::RoundToInt(ParamValue->AsNumber());
                            ParamPin->DefaultValue = FString::FromInt(IntValue);
                            UE_LOG(LogTemp, Display, TEXT("  Set integer parameter '%s' to: %d (string: '%s')"),
                                   *ParamName, IntValue, *ParamPin->DefaultValue);
                        }
                        else if (ParamPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Float)
                        {
                            // For other numeric types
                            float FloatValue = ParamValue->AsNumber();
                            ParamPin->DefaultValue = FString::SanitizeFloat(FloatValue);
                            UE_LOG(LogTemp, Display, TEXT("  Set float parameter '%s' to: %f (string: '%s')"),
                                   *ParamName, FloatValue, *ParamPin->DefaultValue);
                        }
                        else if (ParamPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
                        {
                            bool BoolValue = ParamValue->AsBool();
                            ParamPin->DefaultValue = BoolValue ? TEXT("true") : TEXT("false");
                            UE_LOG(LogTemp, Display, TEXT("  Set boolean parameter '%s' to: %s"),
                                   *ParamName, *ParamPin->DefaultValue);
                        }
                        else if (ParamPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct && ParamPin->PinType.PinSubCategoryObject == TBaseStructure<FVector>::Get())
                        {
                            // Handle array parameters - like Vector parameters
                            const TArray<TSharedPtr<FJsonValue>>* ArrayValue;
                            if (ParamValue->TryGetArray(ArrayValue))
                            {
                                // Check if this could be a vector (array of 3 numbers)
                                if (ArrayValue->Num() == 3)
                                {
                                    // Create a proper vector string: (X=0.0,Y=0.0,Z=1000.0)
                                    float X = (*ArrayValue)[0]->AsNumber();
                                    float Y = (*ArrayValue)[1]->AsNumber();
                                    float Z = (*ArrayValue)[2]->AsNumber();

                                    FString VectorString = FString::Printf(TEXT("(X=%f,Y=%f,Z=%f)"), X, Y, Z);
                                    ParamPin->DefaultValue = VectorString;

                                    UE_LOG(LogTemp, Display, TEXT("  Set vector parameter '%s' to: %s"),
                                           *ParamName, *VectorString);
                                    UE_LOG(LogTemp, Display, TEXT("  Final pin value: '%s'"),
                                           *ParamPin->DefaultValue);
                                }
                                else
                                {
                                    UE_LOG(LogTemp, Warning, TEXT("Array parameter type not fully supported yet"));
                                }
                            }
                        }
                    }
                    else if (ParamValue->Type == EJson::Number)
                    {
                        // Handle integer vs float parameters correctly
                        if (ParamPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int)
                        {
                            // Ensure we're using an integer value (no decimal)
                            int32 IntValue = FMath::RoundToInt(ParamValue->AsNumber());
                            ParamPin->DefaultValue = FString::FromInt(IntValue);
                            UE_LOG(LogTemp, Display, TEXT("  Set integer parameter '%s' to: %d (string: '%s')"),
                                   *ParamName, IntValue, *ParamPin->DefaultValue);
                        }
                        else
                        {
                            // For other numeric types
                            float FloatValue = ParamValue->AsNumber();
                            ParamPin->DefaultValue = FString::SanitizeFloat(FloatValue);
                            UE_LOG(LogTemp, Display, TEXT("  Set float parameter '%s' to: %f (string: '%s')"),
                                   *ParamName, FloatValue, *ParamPin->DefaultValue);
                        }
                    }
                    else if (ParamValue->Type == EJson::Boolean)
                    {
                        bool BoolValue = ParamValue->AsBool();
                        ParamPin->DefaultValue = BoolValue ? TEXT("true") : TEXT("false");
                        UE_LOG(LogTemp, Display, TEXT("  Set boolean parameter '%s' to: %s"),
                               *ParamName, *ParamPin->DefaultValue);
                    }
                    else if (ParamValue->Type == EJson::Array)
                    {
                        UE_LOG(LogTemp, Display, TEXT("  Processing array parameter '%s'"), *ParamName);
                        // Handle array parameters - like Vector parameters
                        const TArray<TSharedPtr<FJsonValue>>* ArrayValue;
                        if (ParamValue->TryGetArray(ArrayValue))
                        {
                            // Check if this could be a vector (array of 3 numbers)
                            if (ArrayValue->Num() == 3 &&
                                (ParamPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct) &&
                                (ParamPin->PinType.PinSubCategoryObject == TBaseStructure<FVector>::Get()))
                            {
                                // Create a proper vector string: (X=0.0,Y=0.0,Z=1000.0)
                                float X = (*ArrayValue)[0]->AsNumber();
                                float Y = (*ArrayValue)[1]->AsNumber();
                                float Z = (*ArrayValue)[2]->AsNumber();

                                FString VectorString = FString::Printf(TEXT("(X=%f,Y=%f,Z=%f)"), X, Y, Z);
                                ParamPin->DefaultValue = VectorString;

                                UE_LOG(LogTemp, Display, TEXT("  Set vector parameter '%s' to: %s"),
                                       *ParamName, *VectorString);
                                UE_LOG(LogTemp, Display, TEXT("  Final pin value: '%s'"),
                                       *ParamPin->DefaultValue);
                            }
                            else
                            {
                                UE_LOG(LogTemp, Warning, TEXT("Array parameter type not fully supported yet"));
                            }
                        }
                    }
                    // Add handling for other types as needed
                }
                else
                {
                    UE_LOG(LogTemp, Warning, TEXT("Parameter pin '%s' not found"), *ParamName);
                }
            }
        }
    }

    // Mark the blueprint as modified
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("node_id"), FunctionNode->NodeGuid.ToString());
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintCallFunctionNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString FunctionName;
    if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'function_name' parameter"));
    }

    FString FunctionClassName;
    if (!Params->TryGetStringField(TEXT("function_class"), FunctionClassName) &&
        !Params->TryGetStringField(TEXT("class_name"), FunctionClassName) &&
        !Params->TryGetStringField(TEXT("target_class"), FunctionClassName) &&
        !Params->TryGetStringField(TEXT("target"), FunctionClassName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'function_class' parameter"));
    }

    UClass* FunctionClass = LoadClassForPin(FunctionClassName);
    if (!FunctionClass)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Function class not found: %s"), *FunctionClassName));
    }

    FString FunctionError;
    UFunction* Function = ResolveFunctionByName(FunctionClass, FunctionName, FunctionError);
    if (!Function)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FunctionError);
    }

    const bool bAllowLatent = GetBoolParam(Params, TEXT("allow_latent"), false);
    const bool bAllowEditorOnly = GetBoolParam(Params, TEXT("allow_editor_only"), false);
    const bool bAllowWildcard = GetBoolParam(Params, TEXT("allow_wildcard"), false);
    const bool bAllowDeprecated = GetBoolParam(Params, TEXT("allow_deprecated"), false);
    const bool bAllowInternal = GetBoolParam(Params, TEXT("allow_internal"), false);
    FString ValidationError;
    if (!ValidateBlueprintCallableFunction(Function, bAllowLatent, bAllowEditorOnly, bAllowWildcard, bAllowDeprecated, bAllowInternal, ValidationError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ValidationError);
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    const UEdGraphSchema_K2* K2Schema = Cast<const UEdGraphSchema_K2>(TargetGraph->GetSchema());
    if (!K2Schema)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get K2 schema"));
    }

    UK2Node_CallFunction* FunctionNode = FUnrealMCPCommonUtils::CreateFunctionCallNode(TargetGraph, Function, NodePosition);
    if (!FunctionNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to create function call node: %s"), *Function->GetPathName()));
    }

    TSharedPtr<FJsonObject> AppliedDefaults = MakeShared<FJsonObject>();
    const TSharedPtr<FJsonObject>* ParamDefaultsObj = nullptr;
    bool bHasParamDefaults = Params->TryGetObjectField(TEXT("param_defaults"), ParamDefaultsObj);
    if (!bHasParamDefaults)
    {
        bHasParamDefaults = Params->TryGetObjectField(TEXT("params"), ParamDefaultsObj);
    }

    if (bHasParamDefaults && ParamDefaultsObj && ParamDefaultsObj->IsValid())
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Param : (*ParamDefaultsObj)->Values)
        {
            UEdGraphPin* ParamPin = FUnrealMCPCommonUtils::FindPin(FunctionNode, Param.Key, EGPD_Input);
            if (!ParamPin)
            {
                FunctionNode->DestroyNode();
                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Function parameter pin not found: %s"), *Param.Key));
            }

            FString AppliedValue;
            FString DefaultError;
            if (!ApplyPinDefaultValueChecked(ParamPin, Param.Value, K2Schema, AppliedValue, DefaultError))
            {
                FunctionNode->DestroyNode();
                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid default value for function parameter '%s': %s"), *Param.Key, *DefaultError));
            }

            FunctionNode->PinDefaultValueChanged(ParamPin);
            AppliedDefaults->SetStringField(Param.Key, AppliedValue);
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = NodeToJson(FunctionNode, true);
    ResultObj->SetStringField(TEXT("function_name"), Function->GetName());
    ResultObj->SetStringField(TEXT("function_class"), Function->GetOuterUClass() ? Function->GetOuterUClass()->GetPathName() : FunctionClass->GetPathName());
    ResultObj->SetBoolField(TEXT("is_static"), Function->HasAnyFunctionFlags(FUNC_Static));
    ResultObj->SetBoolField(TEXT("is_pure"), Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
    ResultObj->SetBoolField(TEXT("is_latent"), Function->HasMetaData(FBlueprintMetadata::MD_Latent));
    ResultObj->SetObjectField(TEXT("param_defaults_applied"), AppliedDefaults);
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintVariable(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString VariableName;
    if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'variable_name' parameter"));
    }

    FString VariableType;
    if (!Params->TryGetStringField(TEXT("variable_type"), VariableType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'variable_type' parameter"));
    }

    // Get optional parameters
    bool IsExposed = false;
    if (Params->HasField(TEXT("is_exposed")))
    {
        IsExposed = Params->GetBoolField(TEXT("is_exposed"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FEdGraphPinType PinType;
    FString TypeError;
    if (!BuildPinTypeFromDescriptor(VariableType, Params, TEXT("type_object"), PinType, TypeError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TypeError);
    }

    FString DefaultValue;
    if (const TSharedPtr<FJsonValue> DefaultJsonValue = Params->TryGetField(TEXT("default_value")))
    {
        FString DefaultError;
        if (!GetPinDefaultStringForTypeChecked(DefaultJsonValue, PinType, DefaultValue, DefaultError))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid default value for variable '%s': %s"), *VariableName, *DefaultError));
        }
    }

    if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*VariableName), PinType, DefaultValue))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to add member variable: %s"), *VariableName));
    }

    // Set variable properties
    FBPVariableDescription* NewVar = nullptr;
    for (FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        if (Variable.VarName == FName(*VariableName))
        {
            NewVar = &Variable;
            break;
        }
    }

    if (NewVar)
    {
        if (Params->HasField(TEXT("category")))
        {
            const FString Category = GetStringParam(Params, TEXT("category"));
            NewVar->Category = FText::FromString(Category);
            FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, FName(*VariableName), nullptr, NewVar->Category, true);
        }
        if (Params->HasField(TEXT("tooltip")))
        {
            const FString Tooltip = GetStringParam(Params, TEXT("tooltip"));
            NewVar->SetMetaData(TEXT("Tooltip"), Tooltip);
            FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, FName(*VariableName), nullptr, TEXT("Tooltip"), Tooltip);
        }
        if (Params->HasField(TEXT("friendly_name")))
        {
            NewVar->FriendlyName = GetStringParam(Params, TEXT("friendly_name"));
        }
        const TSharedPtr<FJsonObject>* MetadataObject = nullptr;
        if (Params->TryGetObjectField(TEXT("metadata"), MetadataObject) && MetadataObject && MetadataObject->IsValid())
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& MetadataPair : (*MetadataObject)->Values)
            {
                FString MetadataValue;
                if (MetadataPair.Value->Type == EJson::String)
                {
                    MetadataValue = MetadataPair.Value->AsString();
                }
                else if (MetadataPair.Value->Type == EJson::Boolean)
                {
                    MetadataValue = MetadataPair.Value->AsBool() ? TEXT("true") : TEXT("false");
                }
                else if (MetadataPair.Value->Type == EJson::Number)
                {
                    MetadataValue = FString::SanitizeFloat(MetadataPair.Value->AsNumber());
                }
                else
                {
                    continue;
                }
                NewVar->SetMetaData(FName(*MetadataPair.Key), MetadataValue);
                FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, FName(*VariableName), nullptr, FName(*MetadataPair.Key), MetadataValue);
            }
        }

        if (IsExposed || GetBoolParam(Params, TEXT("is_editable"), false))
        {
            NewVar->PropertyFlags |= CPF_Edit;
        }
    }

    // Mark the blueprint as modified
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("variable_name"), VariableName);
    ResultObj->SetStringField(TEXT("variable_type"), VariableType);
    ResultObj->SetStringField(TEXT("default_value"), DefaultValue);
    ResultObj->SetObjectField(TEXT("pin_type"), PinTypeToJson(PinType));
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintEventDispatcher(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString DispatcherName;
    if (!Params->TryGetStringField(TEXT("dispatcher_name"), DispatcherName) &&
        !Params->TryGetStringField(TEXT("event_dispatcher_name"), DispatcherName) &&
        !Params->TryGetStringField(TEXT("delegate_name"), DispatcherName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'dispatcher_name' parameter"));
    }

    DispatcherName.TrimStartAndEndInline();
    if (DispatcherName.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'dispatcher_name' cannot be empty"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    const FName DispatcherFName(*DispatcherName);
    if (DispatcherFName.IsNone())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'dispatcher_name' must resolve to a valid FName"));
    }

    if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, DispatcherFName) != INDEX_NONE)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Event dispatcher already exists: %s"), *DispatcherName));
    }
    if (FindBlueprintGraph(Blueprint, FString(), DispatcherName, TEXT("any")))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint graph already exists with dispatcher name: %s"), *DispatcherName));
    }

    const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
    if (!K2Schema)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("K2 schema is unavailable"));
    }

    FEdGraphPinType DelegateType;
    DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, DispatcherFName, DelegateType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to add event dispatcher variable: %s"), *DispatcherName));
    }

    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, DispatcherFName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    auto RollbackDispatcher = [&]()
    {
        if (NewGraph)
        {
            FBlueprintEditorUtils::RemoveGraph(Blueprint, NewGraph);
            NewGraph = nullptr;
        }
        FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, DispatcherFName);
    };

    if (!NewGraph)
    {
        RollbackDispatcher();
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to create event dispatcher signature graph: %s"), *DispatcherName));
    }

    NewGraph->bEditable = false;
    K2Schema->CreateDefaultNodesForGraph(*NewGraph);
    K2Schema->CreateFunctionGraphTerminators(*NewGraph, (UClass*)nullptr);
    K2Schema->AddExtraFunctionFlags(NewGraph, FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
    K2Schema->MarkFunctionEntryAsEditable(NewGraph, true);
    Blueprint->DelegateSignatureGraphs.Add(NewGraph);

    UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(FBlueprintEditorUtils::GetEntryNode(NewGraph));
    if (!EntryNode)
    {
        RollbackDispatcher();
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Event dispatcher signature entry node not found"));
    }

    TArray<TSharedPtr<FJsonValue>> InputValues;
    const TArray<TSharedPtr<FJsonValue>>* InputValuesPtr = nullptr;
    if (Params->TryGetArrayField(TEXT("inputs"), InputValuesPtr) && InputValuesPtr)
    {
        InputValues = *InputValuesPtr;
    }
    else if (Params->TryGetArrayField(TEXT("parameters"), InputValuesPtr) && InputValuesPtr)
    {
        InputValues = *InputValuesPtr;
    }

    TArray<TSharedPtr<FJsonValue>> CreatedInputs;
    for (int32 InputIndex = 0; InputIndex < InputValues.Num(); ++InputIndex)
    {
        const TSharedPtr<FJsonObject>* InputObject = nullptr;
        if (!InputValues[InputIndex].IsValid() || !InputValues[InputIndex]->TryGetObject(InputObject) || !InputObject || !InputObject->IsValid())
        {
            RollbackDispatcher();
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Event dispatcher input %d must be an object"), InputIndex));
        }

        FString ParameterName;
        if (!(*InputObject)->TryGetStringField(TEXT("parameter_name"), ParameterName) &&
            !(*InputObject)->TryGetStringField(TEXT("name"), ParameterName))
        {
            RollbackDispatcher();
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Missing input name for event dispatcher input %d"), InputIndex));
        }
        ParameterName.TrimStartAndEndInline();
        if (ParameterName.IsEmpty())
        {
            RollbackDispatcher();
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Event dispatcher input %d name cannot be empty"), InputIndex));
        }

        FString ParameterType;
        if (!(*InputObject)->TryGetStringField(TEXT("parameter_type"), ParameterType) &&
            !(*InputObject)->TryGetStringField(TEXT("type"), ParameterType))
        {
            RollbackDispatcher();
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Missing input type for event dispatcher input '%s'"), *ParameterName));
        }

        FEdGraphPinType PinType;
        FString TypeError;
        if (!BuildPinTypeFromDescriptor(ParameterType, *InputObject, TEXT("type_object"), PinType, TypeError))
        {
            RollbackDispatcher();
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid input type for event dispatcher input '%s': %s"), *ParameterName, *TypeError));
        }

        const FName ParameterFName(*ParameterName);
        if (ParameterFName.IsNone())
        {
            RollbackDispatcher();
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid input name for event dispatcher input %d"), InputIndex));
        }
        if (FUnrealMCPCommonUtils::FindPin(EntryNode, ParameterName, EGPD_Output))
        {
            RollbackDispatcher();
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Event dispatcher input already exists: %s"), *ParameterName));
        }

        FText PinError;
        if (!EntryNode->CanCreateUserDefinedPin(PinType, EGPD_Output, PinError))
        {
            RollbackDispatcher();
            return FUnrealMCPCommonUtils::CreateErrorResponse(PinError.ToString());
        }

        UEdGraphPin* NewPin = EntryNode->CreateUserDefinedPin(ParameterFName, PinType, EGPD_Output, false);
        if (!NewPin)
        {
            RollbackDispatcher();
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to add event dispatcher input: %s"), *ParameterName));
        }

        TSharedPtr<FJsonObject> CreatedInput = MakeShared<FJsonObject>();
        CreatedInput->SetStringField(TEXT("parameter_name"), ParameterName);
        CreatedInput->SetStringField(TEXT("parameter_type"), ParameterType);
        CreatedInput->SetObjectField(TEXT("pin_type"), PinTypeToJson(PinType));
        CreatedInputs.Add(MakeShared<FJsonValueObject>(CreatedInput));
    }

    if (CreatedInputs.Num() > 0)
    {
        EntryNode->ReconstructNode();
    }

    FBPVariableDescription* DispatcherVariable = nullptr;
    for (FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        if (Variable.VarName == DispatcherFName)
        {
            DispatcherVariable = &Variable;
            break;
        }
    }

    if (DispatcherVariable)
    {
        if (Params->HasField(TEXT("category")))
        {
            const FString Category = GetStringParam(Params, TEXT("category"));
            DispatcherVariable->Category = FText::FromString(Category);
            FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, DispatcherFName, nullptr, DispatcherVariable->Category, true);
        }
        if (Params->HasField(TEXT("tooltip")))
        {
            const FString Tooltip = GetStringParam(Params, TEXT("tooltip"));
            DispatcherVariable->SetMetaData(TEXT("Tooltip"), Tooltip);
            FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, DispatcherFName, nullptr, TEXT("Tooltip"), Tooltip);
        }
        if (Params->HasField(TEXT("friendly_name")))
        {
            DispatcherVariable->FriendlyName = GetStringParam(Params, TEXT("friendly_name"));
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("dispatcher_name"), DispatcherName);
    ResultObj->SetObjectField(TEXT("pin_type"), PinTypeToJson(DelegateType));
    ResultObj->SetObjectField(TEXT("signature_graph"), GraphToJson(Blueprint, NewGraph));
    ResultObj->SetArrayField(TEXT("inputs"), CreatedInputs);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintEventDispatcherCallNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString DispatcherName;
    if (!Params->TryGetStringField(TEXT("dispatcher_name"), DispatcherName) &&
        !Params->TryGetStringField(TEXT("event_dispatcher_name"), DispatcherName) &&
        !Params->TryGetStringField(TEXT("delegate_name"), DispatcherName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'dispatcher_name' parameter"));
    }

    DispatcherName.TrimStartAndEndInline();
    if (DispatcherName.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'dispatcher_name' cannot be empty"));
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    FMulticastDelegateProperty* DelegateProperty = FindBlueprintMulticastDelegateProperty(Blueprint, FName(*DispatcherName));
    if (!DelegateProperty)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Event dispatcher property not found: %s. Compile or recreate the dispatcher before adding a call node."), *DispatcherName));
    }
    if (!DelegateProperty->HasAllPropertyFlags(CPF_BlueprintCallable))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Event dispatcher is not BlueprintCallable: %s"), *DispatcherName));
    }
    if (!DelegateProperty->SignatureFunction)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Event dispatcher signature is not available: %s"), *DispatcherName));
    }

    UK2Node_CallDelegate* CallNode = NewObject<UK2Node_CallDelegate>(TargetGraph);
    if (!CallNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create Event Dispatcher call node"));
    }

    const bool bSelfContext = IsSelfContextForDelegateProperty(Blueprint, DelegateProperty);
    CallNode->SetFromProperty(DelegateProperty, bSelfContext, DelegateProperty->GetOwnerClass());
    if (!CallNode->IsCompatibleWithGraph(TargetGraph))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Event Dispatcher call nodes are only supported in event or function graphs"));
    }

    CallNode->NodePosX = NodePosition.X;
    CallNode->NodePosY = NodePosition.Y;
    TargetGraph->AddNode(CallNode, true);
    CallNode->CreateNewGuid();
    CallNode->PostPlacedNewNode();
    CallNode->AllocateDefaultPins();
    CallNode->ReconstructNode();

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = NodeToJson(CallNode, true);
    ResultObj->SetStringField(TEXT("dispatcher_name"), DispatcherName);
    ResultObj->SetStringField(TEXT("signature_function"), DelegateProperty->SignatureFunction->GetPathName());
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintCustomEventNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString CustomEventName;
    if (!Params->TryGetStringField(TEXT("custom_event_name"), CustomEventName) &&
        !Params->TryGetStringField(TEXT("event_name"), CustomEventName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'custom_event_name' parameter"));
    }

    CustomEventName.TrimStartAndEndInline();
    if (CustomEventName.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'custom_event_name' cannot be empty"));
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    if (GetBlueprintGraphType(Blueprint, TargetGraph) != TEXT("event"))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Custom event nodes are only supported in event graphs"));
    }

    const FName CustomEventFName(*CustomEventName);
    TArray<UK2Node_CustomEvent*> ExistingCustomEvents;
    FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_CustomEvent>(Blueprint, ExistingCustomEvents);
    for (UK2Node_CustomEvent* ExistingCustomEvent : ExistingCustomEvents)
    {
        if (ExistingCustomEvent && ExistingCustomEvent->CustomFunctionName == CustomEventFName)
        {
            // Reuse the existing custom event node; apply call_in_editor update if requested.
            bool bExistingCallInEditor = false;
            if (Params->TryGetBoolField(TEXT("call_in_editor"), bExistingCallInEditor))
            {
                ExistingCustomEvent->Modify();
                ExistingCustomEvent->bCallInEditor = bExistingCallInEditor;
                ExistingCustomEvent->ReconstructNode();
                FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            }
            TSharedPtr<FJsonObject> ExistingResultObj = NodeToJson(ExistingCustomEvent, true);
            ExistingResultObj->SetStringField(TEXT("custom_event_name"), CustomEventName);
            ExistingResultObj->SetBoolField(TEXT("reused_existing"), true);
            ExistingResultObj->SetBoolField(TEXT("call_in_editor"), ExistingCustomEvent->bCallInEditor);
            AddGraphField(ExistingResultObj, Blueprint, ExistingCustomEvent->GetGraph());
            return ExistingResultObj;
        }
    }

    FString SignatureSourceDispatcherName;
    Params->TryGetStringField(TEXT("signature_source_dispatcher_name"), SignatureSourceDispatcherName);
    SignatureSourceDispatcherName.TrimStartAndEndInline();

    const UFunction* SignatureFunction = nullptr;
    if (!SignatureSourceDispatcherName.IsEmpty())
    {
        FMulticastDelegateProperty* DelegateProperty = FindBlueprintMulticastDelegateProperty(Blueprint, FName(*SignatureSourceDispatcherName));
        if (!DelegateProperty)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Signature source Event Dispatcher property not found: %s"), *SignatureSourceDispatcherName));
        }
        SignatureFunction = DelegateProperty->SignatureFunction;
        if (!SignatureFunction)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Signature source Event Dispatcher signature is not available: %s"), *SignatureSourceDispatcherName));
        }
    }

    UK2Node_CustomEvent* CustomEventNode = nullptr;
    if (SignatureFunction)
    {
        CustomEventNode = UK2Node_CustomEvent::CreateFromFunction(NodePosition, TargetGraph, CustomEventName, SignatureFunction, false);
    }
    else
    {
        CustomEventNode = NewObject<UK2Node_CustomEvent>(TargetGraph);
        if (CustomEventNode)
        {
            CustomEventNode->CustomFunctionName = CustomEventFName;
            CustomEventNode->SetFlags(RF_Transactional);
            CustomEventNode->NodePosX = NodePosition.X;
            CustomEventNode->NodePosY = NodePosition.Y;
            TargetGraph->Modify();
            TargetGraph->AddNode(CustomEventNode, true, false);
            CustomEventNode->CreateNewGuid();
            CustomEventNode->PostPlacedNewNode();
            CustomEventNode->AllocateDefaultPins();
        }
    }

    if (!CustomEventNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to create custom event node: %s"), *CustomEventName));
    }

    bool bCallInEditor = false;
    if (Params->TryGetBoolField(TEXT("call_in_editor"), bCallInEditor) && bCallInEditor)
    {
        CustomEventNode->bCallInEditor = true;
    }

    CustomEventNode->ReconstructNode();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = NodeToJson(CustomEventNode, true);
    ResultObj->SetStringField(TEXT("custom_event_name"), CustomEventName);
    ResultObj->SetStringField(TEXT("signature_source_dispatcher_name"), SignatureSourceDispatcherName);
    ResultObj->SetStringField(TEXT("signature_function"), SignatureFunction ? SignatureFunction->GetPathName() : FString());
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintEventDispatcherBindNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString DispatcherName;
    if (!Params->TryGetStringField(TEXT("dispatcher_name"), DispatcherName) &&
        !Params->TryGetStringField(TEXT("event_dispatcher_name"), DispatcherName) &&
        !Params->TryGetStringField(TEXT("delegate_name"), DispatcherName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'dispatcher_name' parameter"));
    }

    DispatcherName.TrimStartAndEndInline();
    if (DispatcherName.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'dispatcher_name' cannot be empty"));
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    FMulticastDelegateProperty* DelegateProperty = FindBlueprintMulticastDelegateProperty(Blueprint, FName(*DispatcherName));
    if (!DelegateProperty)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Event dispatcher property not found: %s. Compile or recreate the dispatcher before adding a bind node."), *DispatcherName));
    }
    if (!DelegateProperty->HasAllPropertyFlags(CPF_BlueprintAssignable))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Event dispatcher is not BlueprintAssignable: %s"), *DispatcherName));
    }
    if (!DelegateProperty->SignatureFunction)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Event dispatcher signature is not available: %s"), *DispatcherName));
    }

    UK2Node_AddDelegate* BindNode = NewObject<UK2Node_AddDelegate>(TargetGraph);
    if (!BindNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create Event Dispatcher bind node"));
    }

    BindNode->SetFromProperty(DelegateProperty, IsSelfContextForDelegateProperty(Blueprint, DelegateProperty), DelegateProperty->GetOwnerClass());
    if (!BindNode->IsCompatibleWithGraph(TargetGraph))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Event Dispatcher bind nodes are only supported in event or function graphs"));
    }

    BindNode->NodePosX = NodePosition.X;
    BindNode->NodePosY = NodePosition.Y;
    TargetGraph->AddNode(BindNode, true);
    BindNode->CreateNewGuid();
    BindNode->PostPlacedNewNode();
    BindNode->AllocateDefaultPins();
    BindNode->ReconstructNode();

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = NodeToJson(BindNode, true);
    ResultObj->SetStringField(TEXT("dispatcher_name"), DispatcherName);
    ResultObj->SetStringField(TEXT("signature_function"), DelegateProperty->SignatureFunction->GetPathName());
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintEventDispatcherUnbindNode(const TSharedPtr<FJsonObject>& Params)
{
    FEventDispatcherNodeRequest Request;
    FString Error;
    if (!ResolveEventDispatcherNodeRequest(Params, TEXT("unbind"), Request, Error))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }

    return CreateEventDispatcherLifecycleNode<UK2Node_RemoveDelegate>(
        Request,
        TEXT("unbind"),
        false,
        true,
        false);
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintEventDispatcherClearNode(const TSharedPtr<FJsonObject>& Params)
{
    FEventDispatcherNodeRequest Request;
    FString Error;
    if (!ResolveEventDispatcherNodeRequest(Params, TEXT("clear"), Request, Error))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }

    return CreateEventDispatcherLifecycleNode<UK2Node_ClearDelegate>(
        Request,
        TEXT("clear"),
        false,
        true,
        false);
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintEventDispatcherAssignNode(const TSharedPtr<FJsonObject>& Params)
{
    FEventDispatcherNodeRequest Request;
    FString Error;
    if (!ResolveEventDispatcherNodeRequest(Params, TEXT("assign"), Request, Error))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
    }

    return CreateEventDispatcherLifecycleNode<UK2Node_AssignDelegate>(
        Request,
        TEXT("assign"),
        true,
        false,
        true);
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintFunctionParameter(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ParameterName;
    if (!Params->TryGetStringField(TEXT("parameter_name"), ParameterName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'parameter_name' parameter"));
    }

    FString ParameterType;
    if (!Params->TryGetStringField(TEXT("parameter_type"), ParameterType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'parameter_type' parameter"));
    }

    FString Direction;
    Params->TryGetStringField(TEXT("direction"), Direction);
    const bool bIsOutputParameter = Direction.Equals(TEXT("output"), ESearchCase::IgnoreCase) || Direction.Equals(TEXT("return"), ESearchCase::IgnoreCase);
    if (!bIsOutputParameter && !Direction.IsEmpty() && !Direction.Equals(TEXT("input"), ESearchCase::IgnoreCase))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'direction' must be 'input' or 'output'"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    if (GetBlueprintGraphType(Blueprint, TargetGraph) != TEXT("function"))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Function parameters are only supported in function graphs"));
    }

    FEdGraphPinType PinType;
    FString TypeError;
    if (!BuildPinTypeFromDescriptor(ParameterType, Params, TEXT("type_object"), PinType, TypeError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TypeError);
    }

    const TSharedPtr<FJsonValue> DefaultJsonValue = Params->TryGetField(TEXT("default_value"));
    FString DefaultValue;
    if (DefaultJsonValue.IsValid())
    {
        FString DefaultError;
        if (!GetPinDefaultStringForTypeChecked(DefaultJsonValue, PinType, DefaultValue, DefaultError))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid default value for function parameter '%s': %s"), *ParameterName, *DefaultError));
        }
    }

    UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(FBlueprintEditorUtils::GetEntryNode(TargetGraph));
    if (!EntryNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Function entry node not found"));
    }

    UK2Node_EditablePinBase* EditableNode = EntryNode;
    EEdGraphPinDirection PinDirection = EGPD_Output;
    if (bIsOutputParameter)
    {
        UK2Node_FunctionResult* ReturnNode = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(EntryNode);
        if (!ReturnNode)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create function return node"));
        }
        EditableNode = ReturnNode;
        PinDirection = EGPD_Input;
    }

    if (FUnrealMCPCommonUtils::FindPin(EditableNode, ParameterName, PinDirection))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Function parameter already exists: %s"), *ParameterName));
    }

    FText PinError;
    if (!EditableNode->CanCreateUserDefinedPin(PinType, PinDirection, PinError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(PinError.ToString());
    }

    UEdGraphPin* NewPin = EditableNode->CreateUserDefinedPin(FName(*ParameterName), PinType, PinDirection, false);
    if (!NewPin)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to add function parameter: %s"), *ParameterName));
    }

    if (DefaultJsonValue.IsValid())
    {
        TSharedPtr<FUserPinInfo> PinInfo = FindUserDefinedPinInfo(EditableNode, FName(*ParameterName), PinDirection);
        if (!PinInfo.IsValid() || !EditableNode->ModifyUserDefinedPinDefaultValue(PinInfo, DefaultValue))
        {
            EditableNode->RemoveUserDefinedPinByName(FName(*ParameterName));
            EditableNode->ReconstructNode();
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to set default value for function parameter: %s"), *ParameterName));
        }
    }

    EditableNode->ReconstructNode();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = NodeToJson(EditableNode, true);
    ResultObj->SetStringField(TEXT("parameter_name"), ParameterName);
    ResultObj->SetStringField(TEXT("direction"), bIsOutputParameter ? TEXT("output") : TEXT("input"));
    ResultObj->SetStringField(TEXT("default_value"), DefaultValue);
    ResultObj->SetObjectField(TEXT("pin_type"), PinTypeToJson(PinType));
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintLocalVariable(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString VariableName;
    if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'variable_name' parameter"));
    }

    FString VariableType;
    if (!Params->TryGetStringField(TEXT("variable_type"), VariableType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'variable_type' parameter"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    if (GetBlueprintGraphType(Blueprint, TargetGraph) != TEXT("function"))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Local variables are only supported in function graphs"));
    }
    if (!FBlueprintEditorUtils::DoesSupportLocalVariables(TargetGraph))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Target graph does not support local variables"));
    }

    FEdGraphPinType PinType;
    FString TypeError;
    if (!BuildPinTypeFromDescriptor(VariableType, Params, TEXT("type_object"), PinType, TypeError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TypeError);
    }

    if (FBlueprintEditorUtils::FindLocalVariable(Blueprint, TargetGraph, FName(*VariableName)))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Local variable already exists: %s"), *VariableName));
    }

    FString DefaultValue;
    if (const TSharedPtr<FJsonValue> DefaultJsonValue = Params->TryGetField(TEXT("default_value")))
    {
        FString DefaultError;
        if (!GetPinDefaultStringForTypeChecked(DefaultJsonValue, PinType, DefaultValue, DefaultError))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid default value for local variable '%s': %s"), *VariableName, *DefaultError));
        }
    }

    if (!FBlueprintEditorUtils::AddLocalVariable(Blueprint, TargetGraph, FName(*VariableName), PinType, DefaultValue))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to add local variable: %s"), *VariableName));
    }

    if (FBPVariableDescription* LocalVariable = FBlueprintEditorUtils::FindLocalVariable(Blueprint, TargetGraph, FName(*VariableName)))
    {
        if (Params->HasField(TEXT("category")))
        {
            LocalVariable->Category = FText::FromString(GetStringParam(Params, TEXT("category")));
        }
        if (Params->HasField(TEXT("tooltip")))
        {
            LocalVariable->SetMetaData(TEXT("Tooltip"), GetStringParam(Params, TEXT("tooltip")));
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("variable_name"), VariableName);
    ResultObj->SetStringField(TEXT("variable_type"), VariableType);
    ResultObj->SetStringField(TEXT("default_value"), DefaultValue);
    ResultObj->SetObjectField(TEXT("pin_type"), PinTypeToJson(PinType));
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintBranchNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    UK2Node_IfThenElse* BranchNode = AddK2NodeToGraph<UK2Node_IfThenElse>(TargetGraph, NodePosition);
    if (!BranchNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create branch node"));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    TSharedPtr<FJsonObject> ResultObj = NodeToJson(BranchNode, true);
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintSequenceNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    int32 OutputCount = 2;
    if (Params->HasField(TEXT("output_count")))
    {
        OutputCount = FMath::Max(2, FMath::RoundToInt(Params->GetNumberField(TEXT("output_count"))));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    UK2Node_ExecutionSequence* SequenceNode = AddK2NodeToGraph<UK2Node_ExecutionSequence>(TargetGraph, NodePosition);
    if (!SequenceNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create sequence node"));
    }

    int32 CurrentExecOutputs = 0;
    for (UEdGraphPin* Pin : SequenceNode->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            ++CurrentExecOutputs;
        }
    }

    while (CurrentExecOutputs < OutputCount)
    {
        SequenceNode->AddInputPin();
        ++CurrentExecOutputs;
    }

    SequenceNode->ReconstructNode();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = NodeToJson(SequenceNode, true);
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintReturnNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FVector2D NodePosition(320.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    if (GetBlueprintGraphType(Blueprint, TargetGraph) != TEXT("function"))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Return nodes are only supported in function graphs"));
    }

    UK2Node_EditablePinBase* EntryNode = FBlueprintEditorUtils::GetEntryNode(TargetGraph);
    if (!EntryNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Function entry node not found"));
    }

    UK2Node_FunctionResult* ReturnNode = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(EntryNode);
    if (!ReturnNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create function return node"));
    }

    ReturnNode->NodePosX = NodePosition.X;
    ReturnNode->NodePosY = NodePosition.Y;
    ReturnNode->ReconstructNode();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = NodeToJson(ReturnNode, true);
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintDynamicCastNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString TargetClassName;
    if (!Params->TryGetStringField(TEXT("target_class"), TargetClassName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'target_class' parameter"));
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UClass* TargetClass = LoadClassForPin(TargetClassName);
    if (!TargetClass || !TargetClass->IsChildOf(UObject::StaticClass()))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Target class not found or not a UObject class: %s"), *TargetClassName));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(TargetGraph);
    if (!CastNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create dynamic cast node"));
    }

    CastNode->TargetType = TargetClass;
    CastNode->NodePosX = NodePosition.X;
    CastNode->NodePosY = NodePosition.Y;
    TargetGraph->AddNode(CastNode, true);
    CastNode->CreateNewGuid();
    CastNode->PostPlacedNewNode();
    CastNode->AllocateDefaultPins();
    if (Params->HasField(TEXT("pure")))
    {
        CastNode->SetPurity(Params->GetBoolField(TEXT("pure")));
    }
    CastNode->ReconstructNode();

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    TSharedPtr<FJsonObject> ResultObj = NodeToJson(CastNode, true);
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintLoopNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    const FString LoopType = GetStringParam(Params, TEXT("loop_type"), TEXT("for_loop"));
    FString LoopTypeError;
    const FName MacroName = ResolveLoopMacroName(LoopType, LoopTypeError);
    if (MacroName.IsNone())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(LoopTypeError);
    }

    FString MacroError;
    UEdGraph* MacroGraph = ResolveStandardMacroGraph(MacroName.ToString(), MacroError);
    if (!MacroGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(MacroError);
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    const UEdGraphSchema_K2* K2Schema = Cast<const UEdGraphSchema_K2>(TargetGraph->GetSchema());
    if (!K2Schema)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get K2 schema"));
    }

    UK2Node_MacroInstance* LoopNode = NewObject<UK2Node_MacroInstance>(TargetGraph);
    if (!LoopNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create loop macro node"));
    }

    LoopNode->SetMacroGraph(MacroGraph);
    LoopNode->NodePosX = NodePosition.X;
    LoopNode->NodePosY = NodePosition.Y;
    TargetGraph->AddNode(LoopNode, true);
    LoopNode->CreateNewGuid();
    LoopNode->PostPlacedNewNode();
    LoopNode->AllocateDefaultPins();

    TSharedPtr<FJsonObject> AppliedDefaults = MakeShared<FJsonObject>();
    FString DefaultError;
    if (!ApplyOptionalIntegerPinDefault(Params, TEXT("first_index"), LoopNode, TEXT("FirstIndex"), K2Schema, AppliedDefaults, DefaultError) ||
        !ApplyOptionalIntegerPinDefault(Params, TEXT("last_index"), LoopNode, TEXT("LastIndex"), K2Schema, AppliedDefaults, DefaultError))
    {
        LoopNode->DestroyNode();
        return FUnrealMCPCommonUtils::CreateErrorResponse(DefaultError);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    TSharedPtr<FJsonObject> ResultObj = NodeToJson(LoopNode, true);
    ResultObj->SetStringField(TEXT("loop_type"), TEXT("for_loop"));
    ResultObj->SetStringField(TEXT("macro_name"), MacroName.ToString());
    ResultObj->SetStringField(TEXT("macro_graph"), MacroGraph->GetPathName());
    ResultObj->SetObjectField(TEXT("pin_defaults_applied"), AppliedDefaults);
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintArrayFunctionNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString Operation;
    if (!Params->TryGetStringField(TEXT("operation"), Operation))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'operation' parameter"));
    }

    FString ElementTypeName;
    if (!Params->TryGetStringField(TEXT("element_type"), ElementTypeName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'element_type' parameter"));
    }

    FString OperationError;
    const FName FunctionName = ResolveArrayOperationFunctionName(Operation, OperationError);
    if (FunctionName.IsNone())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(OperationError);
    }

    FEdGraphPinType ElementPinType;
    FString TypeError;
    if (!BuildPinTypeFromDescriptor(ElementTypeName, Params, TEXT("type_object"), ElementPinType, TypeError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TypeError);
    }
    ElementPinType.ContainerType = EPinContainerType::None;
    ElementPinType.bIsReference = false;

    UFunction* ArrayFunction = UKismetArrayLibrary::StaticClass()->FindFunctionByName(FunctionName);
    if (!ArrayFunction)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Array function not found: %s"), *FunctionName.ToString()));
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    const UEdGraphSchema_K2* K2Schema = Cast<const UEdGraphSchema_K2>(TargetGraph->GetSchema());
    if (!K2Schema)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get K2 schema"));
    }

    UK2Node_CallArrayFunction* ArrayNode = NewObject<UK2Node_CallArrayFunction>(TargetGraph);
    if (!ArrayNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create array function node"));
    }

    ArrayNode->SetFromFunction(ArrayFunction);
    ArrayNode->NodePosX = NodePosition.X;
    ArrayNode->NodePosY = NodePosition.Y;
    TargetGraph->AddNode(ArrayNode, true);
    ArrayNode->CreateNewGuid();
    ArrayNode->PostPlacedNewNode();
    ArrayNode->AllocateDefaultPins();

    FString ArrayTypeError;
    if (!ApplyPinTypeToArrayFunctionNode(ArrayNode, ElementPinType, ArrayTypeError))
    {
        ArrayNode->DestroyNode();
        return FUnrealMCPCommonUtils::CreateErrorResponse(ArrayTypeError);
    }

    TSharedPtr<FJsonObject> AppliedDefaults = MakeShared<FJsonObject>();
    const TSharedPtr<FJsonObject>* ParamDefaultsObj = nullptr;
    bool bHasParamDefaults = Params->TryGetObjectField(TEXT("param_defaults"), ParamDefaultsObj);
    if (!bHasParamDefaults)
    {
        bHasParamDefaults = Params->TryGetObjectField(TEXT("params"), ParamDefaultsObj);
    }

    if (bHasParamDefaults && ParamDefaultsObj && ParamDefaultsObj->IsValid())
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Param : (*ParamDefaultsObj)->Values)
        {
            if (Param.Key.Equals(TEXT("TargetArray"), ESearchCase::IgnoreCase))
            {
                ArrayNode->DestroyNode();
                return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("TargetArray must be connected from an array pin; defaults are not supported"));
            }

            UEdGraphPin* ParamPin = FUnrealMCPCommonUtils::FindPin(ArrayNode, Param.Key, EGPD_Input);
            if (!ParamPin)
            {
                ArrayNode->DestroyNode();
                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Array function parameter pin not found: %s"), *Param.Key));
            }

            FString AppliedValue;
            FString DefaultError;
            if (!ApplyPinDefaultValueChecked(ParamPin, Param.Value, K2Schema, AppliedValue, DefaultError))
            {
                ArrayNode->DestroyNode();
                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid default value for array function parameter '%s': %s"), *Param.Key, *DefaultError));
            }

            ArrayNode->PinDefaultValueChanged(ParamPin);
            AppliedDefaults->SetStringField(Param.Key, AppliedValue);
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    TSharedPtr<FJsonObject> ResultObj = NodeToJson(ArrayNode, true);
    ResultObj->SetStringField(TEXT("operation"), NormalizeTypeName(Operation));
    ResultObj->SetStringField(TEXT("function_name"), ArrayFunction->GetName());
    ResultObj->SetStringField(TEXT("function_class"), UKismetArrayLibrary::StaticClass()->GetPathName());
    ResultObj->SetObjectField(TEXT("element_pin_type"), PinTypeToJson(ElementPinType));
    ResultObj->SetObjectField(TEXT("param_defaults_applied"), AppliedDefaults);
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintMakeArrayNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ElementTypeName;
    if (!Params->TryGetStringField(TEXT("element_type"), ElementTypeName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'element_type' parameter"));
    }

    FEdGraphPinType ElementPinType;
    FString TypeError;
    if (!BuildPinTypeFromDescriptor(ElementTypeName, Params, TEXT("type_object"), ElementPinType, TypeError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TypeError);
    }
    if (ElementPinType.IsArray() || GetBoolParam(Params, TEXT("is_array"), false))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Nested array literals are not supported"));
    }
    ElementPinType.ContainerType = EPinContainerType::None;
    ElementPinType.PinValueType = FEdGraphTerminalType();
    ElementPinType.bIsReference = false;

    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    bool bHasValues = false;
    int32 InputCount = 0;
    FString InputError;
    if (!ResolveMakeArrayInputs(Params, Values, bHasValues, InputCount, InputError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(InputError);
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    const UEdGraphSchema_K2* K2Schema = Cast<const UEdGraphSchema_K2>(TargetGraph->GetSchema());
    if (!K2Schema)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get K2 schema"));
    }

    UK2Node_MakeArray* MakeArrayNode = NewObject<UK2Node_MakeArray>(TargetGraph);
    if (!MakeArrayNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create make array node"));
    }

    MakeArrayNode->NumInputs = InputCount;
    MakeArrayNode->NodePosX = NodePosition.X;
    MakeArrayNode->NodePosY = NodePosition.Y;
    TargetGraph->AddNode(MakeArrayNode, true);
    MakeArrayNode->CreateNewGuid();
    MakeArrayNode->PostPlacedNewNode();
    MakeArrayNode->AllocateDefaultPins();

    FString ArrayTypeError;
    if (!ApplyPinTypeToMakeArrayNode(MakeArrayNode, ElementPinType, ArrayTypeError))
    {
        MakeArrayNode->DestroyNode();
        return FUnrealMCPCommonUtils::CreateErrorResponse(ArrayTypeError);
    }

    TSharedPtr<FJsonObject> AppliedDefaults = MakeShared<FJsonObject>();
    if (bHasValues && Values)
    {
        for (int32 Index = 0; Index < Values->Num(); ++Index)
        {
            const FString PinName = FString::Printf(TEXT("[%d]"), Index);
            UEdGraphPin* InputPin = FUnrealMCPCommonUtils::FindPin(MakeArrayNode, PinName, EGPD_Input);
            if (!InputPin)
            {
                MakeArrayNode->DestroyNode();
                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Make array input pin not found: %s"), *PinName));
            }

            FString AppliedValue;
            FString DefaultError;
            if (!ApplyPinDefaultValueChecked(InputPin, (*Values)[Index], K2Schema, AppliedValue, DefaultError))
            {
                MakeArrayNode->DestroyNode();
                return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid default value for make array pin '%s': %s"), *PinName, *DefaultError));
            }

            MakeArrayNode->PinDefaultValueChanged(InputPin);
            AppliedDefaults->SetStringField(PinName, AppliedValue);
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    TSharedPtr<FJsonObject> ResultObj = NodeToJson(MakeArrayNode, true);
    ResultObj->SetObjectField(TEXT("element_pin_type"), PinTypeToJson(ElementPinType));
    ResultObj->SetNumberField(TEXT("input_count"), InputCount);
    ResultObj->SetObjectField(TEXT("values_applied"), AppliedDefaults);
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintInputActionNode(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString ActionName;
    if (!Params->TryGetStringField(TEXT("action_name"), ActionName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'action_name' parameter"));
    }

    // Get position parameters (optional)
    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    // Create the input action node
    UK2Node_InputAction* InputActionNode = FUnrealMCPCommonUtils::CreateInputActionNode(TargetGraph, ActionName, NodePosition);
    if (!InputActionNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create input action node"));
    }

    // Mark the blueprint as modified
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("node_id"), InputActionNode->NodeGuid.ToString());
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintSelfReference(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    // Get position parameters (optional)
    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    // Create the self node
    UK2Node_Self* SelfNode = FUnrealMCPCommonUtils::CreateSelfReferenceNode(TargetGraph, NodePosition);
    if (!SelfNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create self node"));
    }

    // Mark the blueprint as modified
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("node_id"), SelfNode->NodeGuid.ToString());
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleResolveBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> CandidateArray;

    const TArray<FString> CandidatePaths = FUnrealMCPCommonUtils::FindBlueprintAssetPaths(BlueprintName);
    for (const FString& CandidatePath : CandidatePaths)
    {
        CandidateArray.Add(MakeShared<FJsonValueString>(CandidatePath));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    ResultObj->SetBoolField(TEXT("resolved"), Blueprint != nullptr);
    if (Blueprint)
    {
        ResultObj->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
        ResultObj->SetStringField(TEXT("name"), Blueprint->GetName());
    }
    ResultObj->SetArrayField(TEXT("candidates"), CandidateArray);

    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleListBlueprintGraphs(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString GraphType;
    Params->TryGetStringField(TEXT("graph_type"), GraphType);
    const FString NormalizedType = NormalizeGraphType(GraphType);

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    TArray<UEdGraph*> Graphs;
    Blueprint->GetAllGraphs(Graphs);

    TArray<TSharedPtr<FJsonValue>> GraphArray;
    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph)
        {
            continue;
        }

        const FString CurrentType = GetBlueprintGraphType(Blueprint, Graph);
        if (!NormalizedType.IsEmpty() && NormalizedType != TEXT("any") && CurrentType != NormalizedType)
        {
            continue;
        }

        GraphArray.Add(MakeShared<FJsonValueObject>(GraphToJson(Blueprint, Graph)));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    ResultObj->SetArrayField(TEXT("graphs"), GraphArray);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleResolveBlueprintGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    bool bCreateIfMissing = false;
    if (Params->HasField(TEXT("create_if_missing")))
    {
        bCreateIfMissing = Params->GetBoolField(TEXT("create_if_missing"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    bool bCreated = false;
    FString GraphError;
    UEdGraph* Graph = ResolveBlueprintGraph(Blueprint, Params, bCreateIfMissing, bCreated, GraphError);
    if (!Graph)
    {
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
        ResultObj->SetBoolField(TEXT("resolved"), false);
        ResultObj->SetBoolField(TEXT("created"), false);
        ResultObj->SetStringField(TEXT("error"), GraphError);
        return ResultObj;
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    ResultObj->SetBoolField(TEXT("resolved"), true);
    ResultObj->SetBoolField(TEXT("created"), bCreated);
    AddGraphField(ResultObj, Blueprint, Graph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleListBlueprintNodes(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString NodeType;
    Params->TryGetStringField(TEXT("node_type"), NodeType);

    FString TitleContains;
    Params->TryGetStringField(TEXT("title_contains"), TitleContains);

    bool bIncludePins = true;
    if (Params->HasField(TEXT("include_pins")))
    {
        bIncludePins = Params->GetBoolField(TEXT("include_pins"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    TArray<TSharedPtr<FJsonValue>> Nodes;
    for (UEdGraphNode* Node : TargetGraph->Nodes)
    {
        if (MatchesNodeFilter(Node, NodeType, TitleContains))
        {
            Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(Node, bIncludePins)));
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
    ResultObj->SetStringField(TEXT("graph_name"), TargetGraph->GetName());
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    ResultObj->SetArrayField(TEXT("nodes"), Nodes);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintInputAxisEventNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString AxisName;
    if (!Params->TryGetStringField(TEXT("axis_name"), AxisName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'axis_name' parameter"));
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    UK2Node_InputAxisEvent* AxisNode = NewObject<UK2Node_InputAxisEvent>(TargetGraph);
    if (!AxisNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create input axis event node"));
    }

    AxisNode->Initialize(FName(*AxisName));
    AxisNode->NodePosX = NodePosition.X;
    AxisNode->NodePosY = NodePosition.Y;
    if (Params->HasField(TEXT("consume_input")))
    {
        AxisNode->bConsumeInput = Params->GetBoolField(TEXT("consume_input"));
    }
    if (Params->HasField(TEXT("execute_when_paused")))
    {
        AxisNode->bExecuteWhenPaused = Params->GetBoolField(TEXT("execute_when_paused"));
    }
    if (Params->HasField(TEXT("override_parent_binding")))
    {
        AxisNode->bOverrideParentBinding = Params->GetBoolField(TEXT("override_parent_binding"));
    }

    TargetGraph->AddNode(AxisNode, true);
    AxisNode->CreateNewGuid();
    AxisNode->PostPlacedNewNode();
    AxisNode->AllocateDefaultPins();
    AxisNode->ReconstructNode();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = NodeToJson(AxisNode, true);
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintEnhancedInputActionNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString InputActionPath;
    if (!Params->TryGetStringField(TEXT("input_action"), InputActionPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'input_action' parameter"));
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    UInputAction* InputAction = Cast<UInputAction>(LoadObjectForPin(InputActionPath));
    if (!InputAction)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("InputAction not found: %s"), *InputActionPath));
    }

    UK2Node_EnhancedInputAction* InputActionNode = NewObject<UK2Node_EnhancedInputAction>(TargetGraph);
    if (!InputActionNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create enhanced input action node"));
    }

    InputActionNode->InputAction = InputAction;
    InputActionNode->NodePosX = NodePosition.X;
    InputActionNode->NodePosY = NodePosition.Y;
    TargetGraph->AddNode(InputActionNode, true);
    InputActionNode->CreateNewGuid();
    InputActionNode->PostPlacedNewNode();
    InputActionNode->AllocateDefaultPins();
    InputActionNode->ReconstructNode();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = NodeToJson(InputActionNode, true);
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintVariableGetNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString VariableName;
    if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'variable_name' parameter"));
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    UK2Node_VariableGet* VariableGetNode = FUnrealMCPCommonUtils::CreateVariableGetNode(TargetGraph, Blueprint, VariableName, NodePosition);
    if (!VariableGetNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Variable not found: %s"), *VariableName));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    TSharedPtr<FJsonObject> ResultObj = NodeToJson(VariableGetNode, true);
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintVariableSetNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString VariableName;
    if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'variable_name' parameter"));
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    UK2Node_VariableSet* VariableSetNode = FUnrealMCPCommonUtils::CreateVariableSetNode(TargetGraph, Blueprint, VariableName, NodePosition);
    if (!VariableSetNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Variable not found: %s"), *VariableName));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    TSharedPtr<FJsonObject> ResultObj = NodeToJson(VariableSetNode, true);
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintMathNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString Operation;
    if (!Params->TryGetStringField(TEXT("operation"), Operation))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'operation' parameter"));
    }

    const FName FunctionName = GetMathFunctionName(Operation);
    if (FunctionName == NAME_None)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unsupported math operation: %s"), *Operation));
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    UFunction* Function = UKismetMathLibrary::StaticClass()->FindFunctionByName(FunctionName);
    if (!Function)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Math function not found: %s"), *FunctionName.ToString()));
    }

    UK2Node_CallFunction* MathNode = FUnrealMCPCommonUtils::CreateFunctionCallNode(TargetGraph, Function, NodePosition);
    if (!MathNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create math node"));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    TSharedPtr<FJsonObject> ResultObj = NodeToJson(MathNode, true);
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintCompareNode(const TSharedPtr<FJsonObject>& Params)
{
    FString Operation;
    if (!Params->TryGetStringField(TEXT("operation"), Operation))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'operation' parameter"));
    }

    FString ValueType = TEXT("double");
    Params->TryGetStringField(TEXT("value_type"), ValueType);

    FString MappingError;
    const FName FunctionName = GetCompareFunctionName(Operation, ValueType, MappingError);
    if (FunctionName == NAME_None)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(MappingError);
    }

    return CreateBlueprintFunctionLibraryNode(Params, UKismetMathLibrary::StaticClass(), FunctionName, TEXT("compare"));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintBooleanNode(const TSharedPtr<FJsonObject>& Params)
{
    FString Operation;
    if (!Params->TryGetStringField(TEXT("operation"), Operation))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'operation' parameter"));
    }

    FString MappingError;
    const FName FunctionName = GetBooleanFunctionName(Operation, MappingError);
    if (FunctionName == NAME_None)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(MappingError);
    }

    return CreateBlueprintFunctionLibraryNode(Params, UKismetMathLibrary::StaticClass(), FunctionName, TEXT("boolean"));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintSelectNode(const TSharedPtr<FJsonObject>& Params)
{
    FString ValueType = TEXT("int");
    Params->TryGetStringField(TEXT("value_type"), ValueType);

    FString MappingError;
    const FName FunctionName = GetSelectFunctionName(ValueType, MappingError);
    if (FunctionName == NAME_None)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(MappingError);
    }

    return CreateBlueprintFunctionLibraryNode(Params, UKismetMathLibrary::StaticClass(), FunctionName, TEXT("select"));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintLiteralNode(const TSharedPtr<FJsonObject>& Params)
{
    FString LiteralType;
    if (!Params->TryGetStringField(TEXT("literal_type"), LiteralType) && !Params->TryGetStringField(TEXT("value_type"), LiteralType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'literal_type' parameter"));
    }

    FString MappingError;
    const FName FunctionName = GetLiteralFunctionName(LiteralType, MappingError);
    if (FunctionName == NAME_None)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(MappingError);
    }

    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    UFunction* Function = UKismetSystemLibrary::StaticClass()->FindFunctionByName(FunctionName);
    if (!Function)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Literal function not found: %s"), *FunctionName.ToString()));
    }

    UK2Node_CallFunction* LiteralNode = FUnrealMCPCommonUtils::CreateFunctionCallNode(TargetGraph, Function, NodePosition);
    if (!LiteralNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create literal node"));
    }

    const TSharedPtr<FJsonValue> LiteralValue = Params->TryGetField(TEXT("value"));
    if (LiteralValue.IsValid())
    {
        UEdGraphPin* ValuePin = FUnrealMCPCommonUtils::FindPin(LiteralNode, TEXT("Value"), EGPD_Input);
        const UEdGraphSchema_K2* K2Schema = Cast<const UEdGraphSchema_K2>(TargetGraph->GetSchema());
        if (!ValuePin || !K2Schema)
        {
            LiteralNode->DestroyNode();
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to resolve literal value pin"));
        }

        FString RequestedDefaultValue;
        FString DefaultResolveError;
        if (!GetPinDefaultStringForTypeChecked(LiteralValue, ValuePin->PinType, RequestedDefaultValue, DefaultResolveError))
        {
            LiteralNode->DestroyNode();
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid literal value for type '%s': %s"), *LiteralType, *DefaultResolveError));
        }
        K2Schema->TrySetDefaultValue(*ValuePin, RequestedDefaultValue);
        const FString DefaultError = K2Schema->IsCurrentPinDefaultValid(ValuePin);
        if (!DefaultError.IsEmpty() || !K2Schema->DoesDefaultValueMatch(*ValuePin, RequestedDefaultValue))
        {
            LiteralNode->DestroyNode();
            const FString EffectiveError = DefaultError.IsEmpty()
                ? FString::Printf(TEXT("Requested default was not accepted. requested='%s'"), *RequestedDefaultValue)
                : DefaultError;
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid literal value for type '%s': %s"), *LiteralType, *EffectiveError));
        }

        LiteralNode->PinDefaultValueChanged(ValuePin);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    TSharedPtr<FJsonObject> ResultObj = NodeToJson(LiteralNode, true);
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintEnumLiteralNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    const FString EnumType = GetEnumTypeParam(Params);
    if (EnumType.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing enum type parameter. Use 'enum_type', 'type_object', or 'enum_path'"));
    }

    UEnum* Enum = LoadEnumForPin(EnumType);
    if (!Enum)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Enum not found: %s"), *EnumType));
    }

    FString DefaultValue;
    int32 EnumIndex = INDEX_NONE;
    FString DefaultError;
    if (!ResolveEnumValueDefaultString(Params->TryGetField(TEXT("value")), Enum, DefaultValue, EnumIndex, DefaultError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid enum literal value: %s"), *DefaultError));
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    const UEdGraphSchema_K2* K2Schema = Cast<const UEdGraphSchema_K2>(TargetGraph->GetSchema());
    if (!K2Schema)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get K2 schema"));
    }

    UK2Node_EnumLiteral* EnumLiteralNode = NewObject<UK2Node_EnumLiteral>(TargetGraph);
    if (!EnumLiteralNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create enum literal node"));
    }

    EnumLiteralNode->Enum = Enum;
    EnumLiteralNode->NodePosX = NodePosition.X;
    EnumLiteralNode->NodePosY = NodePosition.Y;

    TargetGraph->AddNode(EnumLiteralNode, true);
    EnumLiteralNode->CreateNewGuid();
    EnumLiteralNode->PostPlacedNewNode();
    EnumLiteralNode->AllocateDefaultPins();
    EnumLiteralNode->ReconstructNode();

    UEdGraphPin* EnumInputPin = FUnrealMCPCommonUtils::FindPin(EnumLiteralNode, UK2Node_EnumLiteral::GetEnumInputPinName().ToString(), EGPD_Input);
    if (!EnumInputPin)
    {
        EnumLiteralNode->DestroyNode();
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to resolve enum literal input pin"));
    }

    K2Schema->TrySetDefaultValue(*EnumInputPin, DefaultValue);
    const FString AppliedDefaultError = K2Schema->IsCurrentPinDefaultValid(EnumInputPin);
    const int32 AppliedEnumIndex = Enum->GetIndexByNameString(EnumInputPin->DefaultValue);
    if (!AppliedDefaultError.IsEmpty() || AppliedEnumIndex != EnumIndex)
    {
        EnumLiteralNode->DestroyNode();
        const FString EffectiveError = AppliedDefaultError.IsEmpty()
            ? FString::Printf(TEXT("Requested enum literal default was not accepted. requested='%s', actual='%s'"), *DefaultValue, *EnumInputPin->DefaultValue)
            : AppliedDefaultError;
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid enum literal value: %s"), *EffectiveError));
    }

    EnumLiteralNode->PinDefaultValueChanged(EnumInputPin);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = NodeToJson(EnumLiteralNode, true);
    ResultObj->SetStringField(TEXT("enum_type"), Enum->GetPathName());
    ResultObj->SetStringField(TEXT("default_value"), DefaultValue);
    ResultObj->SetNumberField(TEXT("default_index"), EnumIndex);
    ResultObj->SetArrayField(TEXT("enum_entries"), EnumEntriesToJson(Enum));
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintIsValidNode(const TSharedPtr<FJsonObject>& Params)
{
    FString ValueType = TEXT("object");
    Params->TryGetStringField(TEXT("value_type"), ValueType);

    FString MappingError;
    const FName FunctionName = GetIsValidFunctionName(ValueType, MappingError);
    if (FunctionName == NAME_None)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(MappingError);
    }

    return CreateBlueprintFunctionLibraryNode(Params, UKismetSystemLibrary::StaticClass(), FunctionName, TEXT("is_valid"));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintMakeStructNode(const TSharedPtr<FJsonObject>& Params)
{
    FString StructType;
    if (!Params->TryGetStringField(TEXT("struct_type"), StructType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'struct_type' parameter"));
    }

    FString MappingError;
    const FName FunctionName = GetStructHelperFunctionName(TEXT("make"), StructType, MappingError);
    if (FunctionName == NAME_None)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(MappingError);
    }

    return CreateBlueprintFunctionLibraryNode(Params, UKismetMathLibrary::StaticClass(), FunctionName, TEXT("make_struct"));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintBreakStructNode(const TSharedPtr<FJsonObject>& Params)
{
    FString StructType;
    if (!Params->TryGetStringField(TEXT("struct_type"), StructType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'struct_type' parameter"));
    }

    FString MappingError;
    const FName FunctionName = GetStructHelperFunctionName(TEXT("break"), StructType, MappingError);
    if (FunctionName == NAME_None)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(MappingError);
    }

    return CreateBlueprintFunctionLibraryNode(Params, UKismetMathLibrary::StaticClass(), FunctionName, TEXT("break_struct"));
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintSwitchIntNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    int32 StartIndex = 0;
    int32 CaseCount = 0;
    FString CaseError;
    if (!ResolveSwitchIntCases(Params, StartIndex, CaseCount, CaseError))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(CaseError);
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    const bool bHasDefaultPin = GetBoolParam(Params, TEXT("has_default_pin"), true);

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    UK2Node_SwitchInteger* SwitchNode = NewObject<UK2Node_SwitchInteger>(TargetGraph);
    if (!SwitchNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create switch int node"));
    }

    SwitchNode->StartIndex = StartIndex;
    SwitchNode->bHasDefaultPin = bHasDefaultPin;
    SwitchNode->NodePosX = NodePosition.X;
    SwitchNode->NodePosY = NodePosition.Y;

    TargetGraph->AddNode(SwitchNode, true);
    SwitchNode->CreateNewGuid();
    SwitchNode->PostPlacedNewNode();
    SwitchNode->AllocateDefaultPins();

    for (int32 CaseIndex = 0; CaseIndex < CaseCount; ++CaseIndex)
    {
        SwitchNode->AddPinToSwitchNode();
    }

    SwitchNode->ReconstructNode();
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = NodeToJson(SwitchNode, true);
    ResultObj->SetNumberField(TEXT("start_index"), StartIndex);
    ResultObj->SetNumberField(TEXT("case_count"), CaseCount);
    ResultObj->SetBoolField(TEXT("has_default_pin"), bHasDefaultPin);
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleAddBlueprintSwitchEnumNode(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    const FString EnumType = GetEnumTypeParam(Params);
    if (EnumType.IsEmpty())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing enum type parameter. Use 'enum_type', 'type_object', or 'enum_path'"));
    }

    UEnum* Enum = LoadEnumForPin(EnumType);
    if (!Enum)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Enum not found: %s"), *EnumType));
    }

    const TArray<TSharedPtr<FJsonValue>> EnumCaseEntries = EnumEntriesToJson(Enum, true);
    if (EnumCaseEntries.Num() == 0)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Enum has no switchable values: %s"), *Enum->GetPathName()));
    }

    FVector2D NodePosition(0.0f, 0.0f);
    if (Params->HasField(TEXT("node_position")))
    {
        NodePosition = FUnrealMCPCommonUtils::GetVector2DFromJson(Params, TEXT("node_position"));
    }

    const bool bHasDefaultPin = GetBoolParam(Params, TEXT("has_default_pin"), false);

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    UK2Node_SwitchEnum* SwitchNode = NewObject<UK2Node_SwitchEnum>(TargetGraph);
    if (!SwitchNode)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create switch enum node"));
    }

    SwitchNode->SetEnum(Enum);
    SwitchNode->bHasDefaultPin = bHasDefaultPin;
    SwitchNode->NodePosX = NodePosition.X;
    SwitchNode->NodePosY = NodePosition.Y;

    TargetGraph->AddNode(SwitchNode, true);
    SwitchNode->CreateNewGuid();
    SwitchNode->PostPlacedNewNode();
    SwitchNode->AllocateDefaultPins();
    SwitchNode->ReconstructNode();

    int32 CasePinCount = 0;
    for (const UEdGraphPin* Pin : SwitchNode->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && Pin->PinName != FName(TEXT("Default")))
        {
            ++CasePinCount;
        }
    }

    if (CasePinCount != EnumCaseEntries.Num())
    {
        SwitchNode->DestroyNode();
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(
            TEXT("Switch enum case pin count mismatch for %s. expected=%d actual=%d"),
            *Enum->GetPathName(),
            EnumCaseEntries.Num(),
            CasePinCount));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = NodeToJson(SwitchNode, true);
    ResultObj->SetStringField(TEXT("enum_type"), Enum->GetPathName());
    ResultObj->SetNumberField(TEXT("case_count"), CasePinCount);
    ResultObj->SetBoolField(TEXT("has_default_pin"), bHasDefaultPin);
    ResultObj->SetArrayField(TEXT("enum_entries"), EnumCaseEntries);
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleSetBlueprintPinDefault(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString NodeId;
    if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'node_id' parameter"));
    }

    FString PinName;
    if (!Params->TryGetStringField(TEXT("pin_name"), PinName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'pin_name' parameter"));
    }

    TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
    if (!Value.IsValid())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'value' parameter"));
    }

    FString Direction;
    Params->TryGetStringField(TEXT("direction"), Direction);

    EEdGraphPinDirection PinDirection = EGPD_MAX;
    if (Direction.Equals(TEXT("input"), ESearchCase::IgnoreCase))
    {
        PinDirection = EGPD_Input;
    }
    else if (Direction.Equals(TEXT("output"), ESearchCase::IgnoreCase))
    {
        PinDirection = EGPD_Output;
    }

    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    UEdGraphNode* Node = FindNodeById(TargetGraph, NodeId);
    if (!Node)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Node not found: %s"), *NodeId));
    }

    UEdGraphPin* Pin = FUnrealMCPCommonUtils::FindPin(Node, PinName, PinDirection);
    if (!Pin)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Pin not found: %s"), *PinName));
    }

    const UEdGraphSchema_K2* K2Schema = Cast<const UEdGraphSchema_K2>(TargetGraph->GetSchema());
    if (!K2Schema)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get K2 schema"));
    }

    const FString PreviousDefaultValue = Pin->DefaultValue;
    UObject* PreviousDefaultObject = Pin->DefaultObject;
    const FText PreviousDefaultTextValue = Pin->DefaultTextValue;
    FString RequestedDefaultValue;
    UObject* RequestedDefaultObject = nullptr;
    bool bRequestedDefaultValue = false;
    bool bRequestedDefaultObject = false;

    if (Value->Type == EJson::String && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class)
    {
        UClass* Class = LoadClassForPin(Value->AsString());
        if (!Class)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Class not found: %s"), *Value->AsString()));
        }
        RequestedDefaultObject = Class;
        bRequestedDefaultObject = true;
        K2Schema->TrySetDefaultObject(*Pin, Class);
    }
    else if (Value->Type == EJson::String && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
    {
        UObject* Object = LoadObjectForPin(Value->AsString());
        if (!Object)
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Object not found: %s"), *Value->AsString()));
        }
        RequestedDefaultObject = Object;
        bRequestedDefaultObject = true;
        K2Schema->TrySetDefaultObject(*Pin, Object);
    }
    else
    {
        FString DefaultResolveError;
        if (!GetPinDefaultStringForTypeChecked(Value, Pin->PinType, RequestedDefaultValue, DefaultResolveError))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid default value for pin '%s': %s"), *PinName, *DefaultResolveError));
        }
        bRequestedDefaultValue = true;
        K2Schema->TrySetDefaultValue(*Pin, RequestedDefaultValue);
    }

    const FString DefaultError = K2Schema->IsCurrentPinDefaultValid(Pin);
    const bool bDefaultValueApplied = !bRequestedDefaultValue || K2Schema->DoesDefaultValueMatch(*Pin, RequestedDefaultValue);
    const bool bDefaultObjectApplied = !bRequestedDefaultObject || Pin->DefaultObject == RequestedDefaultObject;
    if (!DefaultError.IsEmpty() || !bDefaultValueApplied || !bDefaultObjectApplied)
    {
        Pin->DefaultValue = PreviousDefaultValue;
        Pin->DefaultObject = PreviousDefaultObject;
        Pin->DefaultTextValue = PreviousDefaultTextValue;
        const FString EffectiveError = DefaultError.IsEmpty()
            ? FString::Printf(TEXT("Requested default was not accepted. requested='%s', actual='%s'"), *RequestedDefaultValue, *Pin->GetDefaultAsString())
            : DefaultError;
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Invalid default value for pin '%s': %s"), *PinName, *EffectiveError));
    }

    Node->PinDefaultValueChanged(Pin);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("node_id"), NodeId);
    ResultObj->SetObjectField(TEXT("pin"), PinToJson(Pin));
    AddGraphField(ResultObj, Blueprint, TargetGraph);
    return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPBlueprintNodeCommands::HandleFindBlueprintNodes(const TSharedPtr<FJsonObject>& Params)
{
    // Get required parameters
    FString BlueprintName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
    }

    FString NodeType;
    if (!Params->TryGetStringField(TEXT("node_type"), NodeType))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'node_type' parameter"));
    }

    // Find the blueprint
    UBlueprint* Blueprint = FUnrealMCPCommonUtils::FindBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
    }

    FString GraphError;
    UEdGraph* TargetGraph = ResolveBlueprintGraphForNodeCommand(Blueprint, Params, GraphError);
    if (!TargetGraph)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(GraphError);
    }

    // Create a JSON array for the node GUIDs
    TArray<TSharedPtr<FJsonValue>> NodeGuidArray;

    // Filter nodes by the exact requested type
    if (NodeType == TEXT("Event"))
    {
        FString EventName;
        if (!Params->TryGetStringField(TEXT("event_name"), EventName))
        {
            return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'event_name' parameter for Event node search"));
        }

        // Look for nodes with exact event name (e.g., ReceiveBeginPlay)
        for (UEdGraphNode* Node : TargetGraph->Nodes)
        {
            UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
            if (EventNode && EventNode->EventReference.GetMemberName() == FName(*EventName))
            {
                UE_LOG(LogTemp, Display, TEXT("Found event node with name %s: %s"), *EventName, *EventNode->NodeGuid.ToString());
                NodeGuidArray.Add(MakeShared<FJsonValueString>(EventNode->NodeGuid.ToString()));
            }
        }
    }
    // Add other node types as needed (InputAction, etc.)

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("node_guids"), NodeGuidArray);
    AddGraphField(ResultObj, Blueprint, TargetGraph);

    return ResultObj;
}
