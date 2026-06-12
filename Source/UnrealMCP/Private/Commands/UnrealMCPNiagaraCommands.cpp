#include "Commands/UnrealMCPNiagaraCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"

#include "EditorAssetLibrary.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "NiagaraDecalRendererProperties.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraParameterMapHistory.h"
#include "NiagaraRibbonRendererProperties.h"
#include "NiagaraScratchPadContainer.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "NiagaraVolumeRendererProperties.h"
#include "EdGraph/EdGraphPin.h"
#include "UObject/Package.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

namespace
{
constexpr const TCHAR* NiagaraTempGenerationRoot = TEXT("/Game/_MCP_Temp/NiagaraGenerated/");

FString NormalizeNiagaraObjectPathForLoad(const FString& ObjectPath)
{
    FString NormalizedPath = FPackageName::ExportTextPathToObjectPath(ObjectPath).TrimStartAndEnd();
    NormalizedPath.TrimQuotesInline();

    if ((NormalizedPath.StartsWith(TEXT("/Game/")) || NormalizedPath.StartsWith(TEXT("/Engine/"))) && !NormalizedPath.Contains(TEXT(".")))
    {
        const FString AssetName = FPackageName::GetShortName(NormalizedPath);
        NormalizedPath = FString::Printf(TEXT("%s.%s"), *NormalizedPath, *AssetName);
    }

    return NormalizedPath;
}

FString PackagePathFromObjectPath(const FString& ObjectPath)
{
    FString PackagePath = FPackageName::ExportTextPathToObjectPath(ObjectPath).TrimStartAndEnd();
    PackagePath.TrimQuotesInline();
    int32 DotIndex = INDEX_NONE;
    if (PackagePath.FindChar(TEXT('.'), DotIndex))
    {
        PackagePath.LeftInline(DotIndex);
    }
    return PackagePath;
}

bool IsTempGeneratedNiagaraPath(const FString& ObjectPath)
{
    return PackagePathFromObjectPath(ObjectPath).StartsWith(NiagaraTempGenerationRoot);
}

FString NiagaraObjectPathOrEmpty(const UObject* Object)
{
    return Object ? Object->GetPathName() : FString();
}

TSharedPtr<FJsonValue> NumberArrayToJson(std::initializer_list<double> Values)
{
    TArray<TSharedPtr<FJsonValue>> JsonValues;
    for (double Value : Values)
    {
        JsonValues.Add(MakeShared<FJsonValueNumber>(Value));
    }
    return MakeShared<FJsonValueArray>(JsonValues);
}

TSharedPtr<FJsonValue> NiagaraVariableDataToJsonValue(const FNiagaraVariable& Variable)
{
    if (!Variable.IsDataAllocated())
    {
        return MakeShared<FJsonValueNull>();
    }

    const FNiagaraTypeDefinition& TypeDef = Variable.GetType();
    if (TypeDef == FNiagaraTypeDefinition::GetFloatDef())
    {
        return MakeShared<FJsonValueNumber>(Variable.GetValue<float>());
    }
    if (TypeDef == FNiagaraTypeDefinition::GetIntDef())
    {
        return MakeShared<FJsonValueNumber>(Variable.GetValue<int32>());
    }
    if (TypeDef == FNiagaraTypeDefinition::GetBoolDef())
    {
        const FNiagaraBool Value = Variable.GetValue<FNiagaraBool>();
        return MakeShared<FJsonValueBoolean>(Value.GetValue());
    }
    if (TypeDef == FNiagaraTypeDefinition::GetColorDef())
    {
        const FLinearColor Value = Variable.GetValue<FLinearColor>();
        return NumberArrayToJson({ Value.R, Value.G, Value.B, Value.A });
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec2Def())
    {
        const FVector2f Value = Variable.GetValue<FVector2f>();
        return NumberArrayToJson({ Value.X, Value.Y });
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec3Def())
    {
        const FVector3f Value = Variable.GetValue<FVector3f>();
        return NumberArrayToJson({ Value.X, Value.Y, Value.Z });
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec4Def())
    {
        const FVector4f Value = Variable.GetValue<FVector4f>();
        return NumberArrayToJson({ Value.X, Value.Y, Value.Z, Value.W });
    }
    if (TypeDef == FNiagaraTypeDefinition::GetPositionDef())
    {
        const FVector Value = Variable.GetValue<FVector>();
        return NumberArrayToJson({ Value.X, Value.Y, Value.Z });
    }

    return MakeShared<FJsonValueString>(Variable.ToString());
}

bool JsonArrayToDoubles(const TSharedPtr<FJsonValue>& Value, TArray<double>& OutValues)
{
    if (!Value.IsValid() || Value->Type != EJson::Array)
    {
        return false;
    }

    for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
    {
        double NumberValue = 0.0;
        if (!Item.IsValid() || !Item->TryGetNumber(NumberValue))
        {
            return false;
        }
        OutValues.Add(NumberValue);
    }
    return true;
}

FString NiagaraTypeName(const FNiagaraTypeDefinition& TypeDef)
{
    return TypeDef.GetNameText().ToString();
}

FString NiagaraScriptUsageName(ENiagaraScriptUsage Usage)
{
    if (const UEnum* Enum = StaticEnum<ENiagaraScriptUsage>())
    {
        return Enum->GetNameStringByValue(static_cast<int64>(Usage));
    }
    return FString::FromInt(static_cast<int32>(Usage));
}

TSharedPtr<FJsonObject> NiagaraVariableToJsonObject(const FNiagaraVariableBase& Variable)
{
    TSharedPtr<FJsonObject> VariableObject = MakeShared<FJsonObject>();
    VariableObject->SetStringField(TEXT("name"), Variable.GetName().ToString());
    VariableObject->SetStringField(TEXT("type"), NiagaraTypeName(Variable.GetType()));
    return VariableObject;
}

TSharedPtr<FJsonObject> NiagaraVariableWithDataToJsonObject(const FNiagaraVariable& Variable)
{
    TSharedPtr<FJsonObject> VariableObject = NiagaraVariableToJsonObject(Variable);
    VariableObject->SetBoolField(TEXT("has_data"), Variable.IsDataAllocated());
    VariableObject->SetNumberField(TEXT("data_size_bytes"), Variable.GetAllocatedSizeInBytes());
    VariableObject->SetField(TEXT("value"), NiagaraVariableDataToJsonValue(Variable));
    return VariableObject;
}

TArray<TSharedPtr<FJsonValue>> NiagaraVariablesToJson(const TArray<FNiagaraVariable>& Variables)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const FNiagaraVariable& Variable : Variables)
    {
        Values.Add(MakeShared<FJsonValueObject>(NiagaraVariableToJsonObject(Variable)));
    }
    return Values;
}

TArray<TSharedPtr<FJsonValue>> NiagaraVariableBasesToJson(const TArray<FNiagaraVariableBase>& Variables)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const FNiagaraVariableBase& Variable : Variables)
    {
        Values.Add(MakeShared<FJsonValueObject>(NiagaraVariableToJsonObject(Variable)));
    }
    return Values;
}

TArray<TSharedPtr<FJsonValue>> NiagaraScriptUsagesToJson(const TArray<ENiagaraScriptUsage>& Usages)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    for (ENiagaraScriptUsage Usage : Usages)
    {
        Values.Add(MakeShared<FJsonValueString>(NiagaraScriptUsageName(Usage)));
    }
    return Values;
}

TSharedPtr<FJsonObject> GraphPinToJsonObject(const UEdGraphPin* Pin)
{
    TSharedPtr<FJsonObject> PinObject = MakeShared<FJsonObject>();
    if (!Pin)
    {
        return PinObject;
    }

    PinObject->SetStringField(TEXT("name"), Pin->PinName.ToString());
    PinObject->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
    PinObject->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
    PinObject->SetStringField(TEXT("subcategory"), Pin->PinType.PinSubCategory.ToString());
    PinObject->SetStringField(TEXT("default_value"), Pin->DefaultValue);
    PinObject->SetStringField(TEXT("default_object"), NiagaraObjectPathOrEmpty(Pin->DefaultObject));
    PinObject->SetNumberField(TEXT("linked_to_count"), Pin->LinkedTo.Num());
    return PinObject;
}

TSharedPtr<FJsonObject> LinkedPinToJsonObject(const UEdGraphPin* Pin)
{
    TSharedPtr<FJsonObject> PinObject = MakeShared<FJsonObject>();
    if (!Pin)
    {
        return PinObject;
    }

    const UEdGraphNode* OwningNode = Pin->GetOwningNode();
    PinObject->SetStringField(TEXT("pin_name"), Pin->PinName.ToString());
    PinObject->SetStringField(TEXT("node_name"), OwningNode ? OwningNode->GetName() : FString());
    PinObject->SetStringField(TEXT("node_class"), OwningNode ? OwningNode->GetClass()->GetName() : FString());
    PinObject->SetStringField(TEXT("default_value"), Pin->DefaultValue);
    PinObject->SetStringField(TEXT("default_object"), NiagaraObjectPathOrEmpty(Pin->DefaultObject));
    return PinObject;
}

TArray<TSharedPtr<FJsonValue>> LinkedPinsToJson(const UEdGraphPin* Pin, bool bIncludeLinkedSources)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    if (!Pin || !bIncludeLinkedSources)
    {
        return Values;
    }

    for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
    {
        Values.Add(MakeShared<FJsonValueObject>(LinkedPinToJsonObject(LinkedPin)));
    }
    return Values;
}

TArray<TSharedPtr<FJsonValue>> GraphPinsToJson(const TArray<UEdGraphPin*>& Pins)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const UEdGraphPin* Pin : Pins)
    {
        Values.Add(MakeShared<FJsonValueObject>(GraphPinToJsonObject(Pin)));
    }
    return Values;
}

bool IsSettableNiagaraUserParameterType(const FNiagaraTypeDefinition& TypeDef)
{
    return TypeDef == FNiagaraTypeDefinition::GetFloatDef() ||
        TypeDef == FNiagaraTypeDefinition::GetIntDef() ||
        TypeDef == FNiagaraTypeDefinition::GetBoolDef() ||
        TypeDef == FNiagaraTypeDefinition::GetColorDef() ||
        TypeDef == FNiagaraTypeDefinition::GetVec2Def() ||
        TypeDef == FNiagaraTypeDefinition::GetVec3Def() ||
        TypeDef == FNiagaraTypeDefinition::GetVec4Def() ||
        TypeDef == FNiagaraTypeDefinition::GetPositionDef();
}

TSharedPtr<FJsonValue> NiagaraParameterValueToJson(const FNiagaraUserRedirectionParameterStore& Store, const FNiagaraVariable& Parameter)
{
    const FNiagaraTypeDefinition& TypeDef = Parameter.GetType();
    if (TypeDef == FNiagaraTypeDefinition::GetFloatDef())
    {
        return MakeShared<FJsonValueNumber>(Store.GetParameterValue<float>(Parameter));
    }
    if (TypeDef == FNiagaraTypeDefinition::GetIntDef())
    {
        return MakeShared<FJsonValueNumber>(Store.GetParameterValue<int32>(Parameter));
    }
    if (TypeDef == FNiagaraTypeDefinition::GetBoolDef())
    {
        const FNiagaraBool Value = Store.GetParameterValue<FNiagaraBool>(Parameter);
        return MakeShared<FJsonValueBoolean>(Value.GetValue());
    }
    if (TypeDef == FNiagaraTypeDefinition::GetColorDef())
    {
        const FLinearColor Value = Store.GetParameterValue<FLinearColor>(Parameter);
        return NumberArrayToJson({ Value.R, Value.G, Value.B, Value.A });
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec2Def())
    {
        const FVector2f Value = Store.GetParameterValue<FVector2f>(Parameter);
        return NumberArrayToJson({ Value.X, Value.Y });
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec3Def())
    {
        const FVector3f Value = Store.GetParameterValue<FVector3f>(Parameter);
        return NumberArrayToJson({ Value.X, Value.Y, Value.Z });
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec4Def())
    {
        const FVector4f Value = Store.GetParameterValue<FVector4f>(Parameter);
        return NumberArrayToJson({ Value.X, Value.Y, Value.Z, Value.W });
    }
    if (TypeDef == FNiagaraTypeDefinition::GetPositionDef())
    {
        const FVector* Value = Store.GetPositionParameterValue(Parameter.GetName());
        if (Value)
        {
            return NumberArrayToJson({ Value->X, Value->Y, Value->Z });
        }
    }
    return MakeShared<FJsonValueNull>();
}

bool SetNiagaraUserParameterValue(
    FNiagaraUserRedirectionParameterStore& Store,
    const FNiagaraVariable& Parameter,
    const TSharedPtr<FJsonValue>& JsonValue,
    FString& OutError)
{
    const FNiagaraTypeDefinition& TypeDef = Parameter.GetType();
    if (TypeDef == FNiagaraTypeDefinition::GetFloatDef())
    {
        double NumberValue = 0.0;
        if (!JsonValue.IsValid() || !JsonValue->TryGetNumber(NumberValue))
        {
            OutError = TEXT("Expected numeric value for float Niagara user parameter");
            return false;
        }
        return Store.SetParameterValue<float>(static_cast<float>(NumberValue), Parameter, false);
    }
    if (TypeDef == FNiagaraTypeDefinition::GetIntDef())
    {
        double NumberValue = 0.0;
        if (!JsonValue.IsValid() || !JsonValue->TryGetNumber(NumberValue))
        {
            OutError = TEXT("Expected numeric value for int Niagara user parameter");
            return false;
        }
        return Store.SetParameterValue<int32>(static_cast<int32>(NumberValue), Parameter, false);
    }
    if (TypeDef == FNiagaraTypeDefinition::GetBoolDef())
    {
        bool BoolValue = false;
        if (!JsonValue.IsValid() || !JsonValue->TryGetBool(BoolValue))
        {
            OutError = TEXT("Expected boolean value for bool Niagara user parameter");
            return false;
        }
        return Store.SetParameterValue<FNiagaraBool>(FNiagaraBool(BoolValue), Parameter, false);
    }

    TArray<double> Values;
    if (!JsonArrayToDoubles(JsonValue, Values))
    {
        OutError = TEXT("Expected numeric array value for vector/color Niagara user parameter");
        return false;
    }

    if (TypeDef == FNiagaraTypeDefinition::GetColorDef())
    {
        if (Values.Num() < 3)
        {
            OutError = TEXT("Expected color array with at least 3 values");
            return false;
        }
        const float Alpha = Values.Num() >= 4 ? static_cast<float>(Values[3]) : 1.0f;
        return Store.SetParameterValue<FLinearColor>(
            FLinearColor(static_cast<float>(Values[0]), static_cast<float>(Values[1]), static_cast<float>(Values[2]), Alpha),
            Parameter,
            false);
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec2Def())
    {
        if (Values.Num() < 2)
        {
            OutError = TEXT("Expected vec2 array with 2 values");
            return false;
        }
        return Store.SetParameterValue<FVector2f>(FVector2f(Values[0], Values[1]), Parameter, false);
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec3Def())
    {
        if (Values.Num() < 3)
        {
            OutError = TEXT("Expected vec3 array with 3 values");
            return false;
        }
        return Store.SetParameterValue<FVector3f>(FVector3f(Values[0], Values[1], Values[2]), Parameter, false);
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec4Def())
    {
        if (Values.Num() < 4)
        {
            OutError = TEXT("Expected vec4 array with 4 values");
            return false;
        }
        return Store.SetParameterValue<FVector4f>(FVector4f(Values[0], Values[1], Values[2], Values[3]), Parameter, false);
    }
    if (TypeDef == FNiagaraTypeDefinition::GetPositionDef())
    {
        if (Values.Num() < 3)
        {
            OutError = TEXT("Expected position array with 3 values");
            return false;
        }
        return Store.SetPositionParameterValue(FVector(Values[0], Values[1], Values[2]), Parameter.GetName(), false);
    }

    OutError = FString::Printf(TEXT("Unsupported Niagara user parameter type: %s"), *NiagaraTypeName(TypeDef));
    return false;
}

bool SetNiagaraVariableDataFromJson(FNiagaraVariable& Variable, const TSharedPtr<FJsonValue>& JsonValue, FString& OutError)
{
    const FNiagaraTypeDefinition& TypeDef = Variable.GetType();
    if (TypeDef == FNiagaraTypeDefinition::GetFloatDef())
    {
        double NumberValue = 0.0;
        if (!JsonValue.IsValid() || !JsonValue->TryGetNumber(NumberValue))
        {
            OutError = TEXT("Expected numeric value for float Niagara module input");
            return false;
        }
        Variable.SetValue<float>(static_cast<float>(NumberValue));
        return true;
    }
    if (TypeDef == FNiagaraTypeDefinition::GetIntDef())
    {
        double NumberValue = 0.0;
        if (!JsonValue.IsValid() || !JsonValue->TryGetNumber(NumberValue))
        {
            OutError = TEXT("Expected numeric value for int Niagara module input");
            return false;
        }
        Variable.SetValue<int32>(static_cast<int32>(NumberValue));
        return true;
    }
    if (TypeDef == FNiagaraTypeDefinition::GetBoolDef())
    {
        bool BoolValue = false;
        if (!JsonValue.IsValid() || !JsonValue->TryGetBool(BoolValue))
        {
            OutError = TEXT("Expected boolean value for bool Niagara module input");
            return false;
        }
        Variable.SetValue<FNiagaraBool>(FNiagaraBool(BoolValue));
        return true;
    }

    TArray<double> Values;
    if (!JsonArrayToDoubles(JsonValue, Values))
    {
        OutError = TEXT("Expected numeric array value for vector/color Niagara module input");
        return false;
    }

    if (TypeDef == FNiagaraTypeDefinition::GetColorDef())
    {
        if (Values.Num() < 3)
        {
            OutError = TEXT("Expected color array with at least 3 values");
            return false;
        }
        const float Alpha = Values.Num() >= 4 ? static_cast<float>(Values[3]) : 1.0f;
        Variable.SetValue<FLinearColor>(FLinearColor(static_cast<float>(Values[0]), static_cast<float>(Values[1]), static_cast<float>(Values[2]), Alpha));
        return true;
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec2Def())
    {
        if (Values.Num() < 2)
        {
            OutError = TEXT("Expected vec2 array with 2 values");
            return false;
        }
        Variable.SetValue<FVector2f>(FVector2f(Values[0], Values[1]));
        return true;
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec3Def())
    {
        if (Values.Num() < 3)
        {
            OutError = TEXT("Expected vec3 array with 3 values");
            return false;
        }
        Variable.SetValue<FVector3f>(FVector3f(Values[0], Values[1], Values[2]));
        return true;
    }
    if (TypeDef == FNiagaraTypeDefinition::GetVec4Def())
    {
        if (Values.Num() < 4)
        {
            OutError = TEXT("Expected vec4 array with 4 values");
            return false;
        }
        Variable.SetValue<FVector4f>(FVector4f(Values[0], Values[1], Values[2], Values[3]));
        return true;
    }
    if (TypeDef == FNiagaraTypeDefinition::GetPositionDef())
    {
        if (Values.Num() < 3)
        {
            OutError = TEXT("Expected position array with 3 values");
            return false;
        }
        Variable.SetValue<FVector>(FVector(Values[0], Values[1], Values[2]));
        return true;
    }

    OutError = FString::Printf(TEXT("Unsupported Niagara module input type: %s"), *NiagaraTypeName(TypeDef));
    return false;
}

TArray<TSharedPtr<FJsonValue>> MaterialArrayToJson(const TArray<UMaterialInterface*>& Materials)
{
    TArray<TSharedPtr<FJsonValue>> MaterialValues;
    for (const UMaterialInterface* Material : Materials)
    {
        MaterialValues.Add(MakeShared<FJsonValueString>(NiagaraObjectPathOrEmpty(Material)));
    }
    return MaterialValues;
}

FString RendererMaterialField(const UNiagaraRendererProperties* Renderer)
{
    if (Renderer->IsA<UNiagaraSpriteRendererProperties>())
    {
        return TEXT("Material");
    }
    if (Renderer->IsA<UNiagaraRibbonRendererProperties>())
    {
        return TEXT("Material");
    }
    if (Renderer->IsA<UNiagaraMeshRendererProperties>())
    {
        return TEXT("OverrideMaterials[material_slot].ExplicitMat");
    }
    if (Renderer->IsA<UNiagaraDecalRendererProperties>() || Renderer->IsA<UNiagaraVolumeRendererProperties>())
    {
        return TEXT("Material");
    }
    return FString();
}

bool SetRendererMaterial(
    UNiagaraRendererProperties* Renderer,
    UMaterialInterface* Material,
    int32 MaterialSlotIndex,
    FString& OutChangedField,
    FString& OutError)
{
    if (!Renderer)
    {
        OutError = TEXT("Renderer is null");
        return false;
    }

    if (!Material)
    {
        OutError = TEXT("Material is null");
        return false;
    }

    if (UNiagaraSpriteRendererProperties* SpriteRenderer = Cast<UNiagaraSpriteRendererProperties>(Renderer))
    {
        SpriteRenderer->Modify();
        SpriteRenderer->Material = Material;
        SpriteRenderer->MICMaterial = nullptr;
        OutChangedField = TEXT("Material");
        return true;
    }

    if (UNiagaraRibbonRendererProperties* RibbonRenderer = Cast<UNiagaraRibbonRendererProperties>(Renderer))
    {
        RibbonRenderer->Modify();
        RibbonRenderer->Material = Material;
        RibbonRenderer->MICMaterial = nullptr;
        OutChangedField = TEXT("Material");
        return true;
    }

    if (UNiagaraMeshRendererProperties* MeshRenderer = Cast<UNiagaraMeshRendererProperties>(Renderer))
    {
        const int32 SafeMaterialSlot = FMath::Max(0, MaterialSlotIndex);
        MeshRenderer->Modify();
        MeshRenderer->bOverrideMaterials = true;
        if (MeshRenderer->OverrideMaterials.Num() <= SafeMaterialSlot)
        {
            MeshRenderer->OverrideMaterials.SetNum(SafeMaterialSlot + 1);
        }
        MeshRenderer->OverrideMaterials[SafeMaterialSlot].ExplicitMat = Material;
        OutChangedField = FString::Printf(TEXT("OverrideMaterials[%d].ExplicitMat"), SafeMaterialSlot);
        return true;
    }

    if (UNiagaraDecalRendererProperties* DecalRenderer = Cast<UNiagaraDecalRendererProperties>(Renderer))
    {
        DecalRenderer->Modify();
        DecalRenderer->Material = Material;
        DecalRenderer->MICMaterial = nullptr;
        OutChangedField = TEXT("Material");
        return true;
    }

    if (UNiagaraVolumeRendererProperties* VolumeRenderer = Cast<UNiagaraVolumeRendererProperties>(Renderer))
    {
        VolumeRenderer->Modify();
        VolumeRenderer->Material = Material;
        VolumeRenderer->MICMaterial = nullptr;
        OutChangedField = TEXT("Material");
        return true;
    }

    OutError = FString::Printf(TEXT("Unsupported Niagara renderer class: %s"), *Renderer->GetClass()->GetName());
    return false;
}

void AddUniqueString(TArray<FString>& Values, const FString& Value)
{
    if (!Value.IsEmpty() && !Values.Contains(Value))
    {
        Values.Add(Value);
    }
}

TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values)
{
    TArray<TSharedPtr<FJsonValue>> JsonValues;
    for (const FString& Value : Values)
    {
        JsonValues.Add(MakeShared<FJsonValueString>(Value));
    }
    return JsonValues;
}

void AddHintIfContains(const FString& SearchText, const FString& Needle, const FString& Hint, TArray<FString>& Hints)
{
    if (SearchText.Contains(Needle, ESearchCase::IgnoreCase))
    {
        AddUniqueString(Hints, Hint);
    }
}

TArray<FString> BuildNiagaraControlHints(const FString& SearchText)
{
    TArray<FString> Hints;
    AddHintIfContains(SearchText, TEXT("color"), TEXT("color_or_tint_control"), Hints);
    AddHintIfContains(SearchText, TEXT("colour"), TEXT("color_or_tint_control"), Hints);
    AddHintIfContains(SearchText, TEXT("tint"), TEXT("color_or_tint_control"), Hints);
    AddHintIfContains(SearchText, TEXT("ribbonwidth"), TEXT("ribbon_width_control"), Hints);
    AddHintIfContains(SearchText, TEXT("ribbon width"), TEXT("ribbon_width_control"), Hints);
    AddHintIfContains(SearchText, TEXT("width"), TEXT("width_control"), Hints);
    AddHintIfContains(SearchText, TEXT("scale"), TEXT("scale_control"), Hints);
    AddHintIfContains(SearchText, TEXT("size"), TEXT("size_control"), Hints);
    AddHintIfContains(SearchText, TEXT("lifetime"), TEXT("lifetime_control"), Hints);
    AddHintIfContains(SearchText, TEXT("life time"), TEXT("lifetime_control"), Hints);
    AddHintIfContains(SearchText, TEXT("duration"), TEXT("duration_control"), Hints);
    AddHintIfContains(SearchText, TEXT("velocity"), TEXT("velocity_control"), Hints);
    AddHintIfContains(SearchText, TEXT("speed"), TEXT("velocity_control"), Hints);
    AddHintIfContains(SearchText, TEXT("spawn"), TEXT("spawn_control"), Hints);
    AddHintIfContains(SearchText, TEXT("materialparam"), TEXT("dynamic_material_parameter_control"), Hints);
    AddHintIfContains(SearchText, TEXT("dynamicmaterial"), TEXT("dynamic_material_parameter_control"), Hints);
    AddHintIfContains(SearchText, TEXT("user."), TEXT("user_parameter_reference"), Hints);
    AddHintIfContains(SearchText, TEXT("scratch"), TEXT("scratch_pad_reference"), Hints);
    AddHintIfContains(SearchText, TEXT("trail"), TEXT("trail_behavior"), Hints);
    AddHintIfContains(SearchText, TEXT("ribbon"), TEXT("ribbon_behavior"), Hints);
    return Hints;
}

FString InferNiagaraInputControlKind(const FString& FunctionName, const FString& PinName, const FString& PinCategory)
{
    const FString SearchText = FString::Printf(TEXT("%s %s %s"), *FunctionName, *PinName, *PinCategory);

    if (SearchText.Contains(TEXT("dynamicmaterial"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("materialparam"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("material parameter"), ESearchCase::IgnoreCase))
    {
        return TEXT("dynamic_material_parameter");
    }
    if (SearchText.Contains(TEXT("color"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("colour"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("tint"), ESearchCase::IgnoreCase))
    {
        return TEXT("color");
    }
    if (SearchText.Contains(TEXT("ribbonwidth"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("ribbon width"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("width"), ESearchCase::IgnoreCase))
    {
        return TEXT("width");
    }
    if (SearchText.Contains(TEXT("scale"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("size"), ESearchCase::IgnoreCase))
    {
        return TEXT("scale_or_size");
    }
    if (SearchText.Contains(TEXT("velocity"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("speed"), ESearchCase::IgnoreCase))
    {
        return TEXT("velocity");
    }
    if (SearchText.Contains(TEXT("lifetime"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("life time"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("duration"), ESearchCase::IgnoreCase))
    {
        return TEXT("lifetime");
    }
    if (SearchText.Contains(TEXT("spawn"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("count"), ESearchCase::IgnoreCase) ||
        SearchText.Contains(TEXT("rate"), ESearchCase::IgnoreCase))
    {
        return TEXT("spawn");
    }
    if (SearchText.Contains(TEXT("user."), ESearchCase::IgnoreCase))
    {
        return TEXT("user_parameter_reference");
    }
    if (PinCategory.Contains(TEXT("float"), ESearchCase::IgnoreCase) ||
        PinCategory.Contains(TEXT("double"), ESearchCase::IgnoreCase) ||
        PinCategory.Contains(TEXT("int"), ESearchCase::IgnoreCase) ||
        PinCategory.Contains(TEXT("bool"), ESearchCase::IgnoreCase) ||
        PinCategory.Contains(TEXT("struct"), ESearchCase::IgnoreCase))
    {
        return TEXT("numeric_or_struct");
    }
    return TEXT("unknown");
}

int32 NiagaraInputControlPriority(const FString& ControlKind, const FString& FunctionName, const FString& PinName)
{
    int32 Priority = 10;
    if (ControlKind == TEXT("color") ||
        ControlKind == TEXT("dynamic_material_parameter") ||
        ControlKind == TEXT("scale_or_size") ||
        ControlKind == TEXT("width"))
    {
        Priority = 80;
    }
    else if (ControlKind == TEXT("velocity") ||
        ControlKind == TEXT("spawn") ||
        ControlKind == TEXT("lifetime"))
    {
        Priority = 75;
    }
    else if (ControlKind == TEXT("user_parameter_reference"))
    {
        Priority = 70;
    }
    else if (ControlKind == TEXT("numeric_or_struct"))
    {
        Priority = 35;
    }

    if (FunctionName.Contains(TEXT("ScaleColor"), ESearchCase::IgnoreCase) ||
        FunctionName.Contains(TEXT("DynamicMaterial"), ESearchCase::IgnoreCase))
    {
        Priority += 25;
    }
    else if (FunctionName.Contains(TEXT("AddVelocity"), ESearchCase::IgnoreCase) ||
        FunctionName.Contains(TEXT("SpawnBurst"), ESearchCase::IgnoreCase))
    {
        Priority += 18;
    }
    else if (FunctionName.Contains(TEXT("ScaleMesh"), ESearchCase::IgnoreCase))
    {
        Priority += 15;
    }
    else if (FunctionName.Equals(TEXT("EmitterState"), ESearchCase::IgnoreCase))
    {
        Priority -= 35;
    }

    if (PinName.StartsWith(TEXT("Use "), ESearchCase::IgnoreCase) ||
        PinName.StartsWith(TEXT("Use"), ESearchCase::IgnoreCase))
    {
        Priority -= 10;
    }
    if (PinName.Contains(TEXT("Curve"), ESearchCase::IgnoreCase) && FunctionName.Contains(TEXT("ScaleColor"), ESearchCase::IgnoreCase))
    {
        Priority += 8;
    }

    return FMath::Clamp(Priority, 0, 100);
}

TSharedPtr<FJsonObject> BuildFunctionCallJson(const UNiagaraNodeFunctionCall* FunctionCall, int32 NodeIndex, bool bIncludePins);

bool IsLowSignalNiagaraInputPinName(const FString& PinName)
{
    return PinName.Equals(TEXT("InputMap"), ESearchCase::IgnoreCase) ||
        PinName.Equals(TEXT("OutputMap"), ESearchCase::IgnoreCase) ||
        PinName.Contains(TEXT("Parameter Map"), ESearchCase::IgnoreCase) ||
        PinName.Contains(TEXT("ParameterMap"), ESearchCase::IgnoreCase) ||
        PinName.Contains(TEXT("Write Parameter Index"), ESearchCase::IgnoreCase);
}

bool IsCandidateNiagaraInputPin(const UEdGraphPin* Pin)
{
    if (!Pin || Pin->Direction != EGPD_Input)
    {
        return false;
    }

    const FString PinName = Pin->PinName.ToString();
    const FString PinCategory = Pin->PinType.PinCategory.ToString();
    if (PinCategory.Equals(TEXT("exec"), ESearchCase::IgnoreCase) ||
        IsLowSignalNiagaraInputPinName(PinName))
    {
        return false;
    }

    return true;
}

TSharedPtr<FJsonObject> BuildModuleInputCandidateJson(
    const UNiagaraNodeFunctionCall* FunctionCall,
    const UEdGraphPin* Pin,
    const FString& EmitterName,
    int32 EmitterIndex,
    int32 ModuleIndex,
    bool bIncludeLinkedSources)
{
    TSharedPtr<FJsonObject> CandidateObject = MakeShared<FJsonObject>();
    if (!FunctionCall || !Pin)
    {
        return CandidateObject;
    }

    FString FunctionName = FunctionCall->GetFunctionName();
    if (FunctionName.IsEmpty())
    {
        FunctionName = FunctionCall->Signature.GetNameString();
    }

    const FString PinName = Pin->PinName.ToString();
    const FString PinCategory = Pin->PinType.PinCategory.ToString();
    const FString ControlKind = InferNiagaraInputControlKind(FunctionName, PinName, PinCategory);

    CandidateObject->SetStringField(TEXT("emitter_name"), EmitterName);
    CandidateObject->SetNumberField(TEXT("emitter_index"), EmitterIndex);
    CandidateObject->SetNumberField(TEXT("module_index"), ModuleIndex);
    CandidateObject->SetStringField(TEXT("module_name"), FunctionName);
    CandidateObject->SetStringField(TEXT("module_node_name"), FunctionCall->GetName());
    CandidateObject->SetStringField(TEXT("module_node_guid"), FunctionCall->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
    CandidateObject->SetStringField(TEXT("pin_name"), PinName);
    CandidateObject->SetStringField(TEXT("pin_category"), PinCategory);
    CandidateObject->SetStringField(TEXT("pin_subcategory"), Pin->PinType.PinSubCategory.ToString());
    CandidateObject->SetStringField(TEXT("default_value"), Pin->DefaultValue);
    CandidateObject->SetStringField(TEXT("default_object"), NiagaraObjectPathOrEmpty(Pin->DefaultObject));
    CandidateObject->SetNumberField(TEXT("linked_to_count"), Pin->LinkedTo.Num());
    CandidateObject->SetArrayField(TEXT("linked_sources"), LinkedPinsToJson(Pin, bIncludeLinkedSources));
    CandidateObject->SetStringField(TEXT("control_kind"), ControlKind);
    CandidateObject->SetNumberField(TEXT("priority"), NiagaraInputControlPriority(ControlKind, FunctionName, PinName));
    CandidateObject->SetBoolField(TEXT("can_author_now"), false);
    CandidateObject->SetStringField(TEXT("authoring_status"), TEXT("read_only_candidate; module input writing is intentionally not enabled yet"));
    return CandidateObject;
}

UNiagaraScript* FindEmitterScriptForUsage(FVersionedNiagaraEmitterData* EmitterData, ENiagaraScriptUsage Usage)
{
    if (!EmitterData)
    {
        return nullptr;
    }

    TArray<UNiagaraScript*> EmitterScripts;
    EmitterData->GetScripts(EmitterScripts, false, false);
    for (UNiagaraScript* Script : EmitterScripts)
    {
        if (Script && Script->GetUsage() == Usage)
        {
            return Script;
        }
    }
    return nullptr;
}

FNiagaraVariable BuildRapidIterationParameterForInput(
    const UNiagaraNodeFunctionCall* FunctionCall,
    const FNiagaraVariable& InputVariable,
    const FString& EmitterName,
    ENiagaraScriptUsage ScriptUsage)
{
    if (!FunctionCall || EmitterName.IsEmpty())
    {
        return FNiagaraVariable();
    }

    const FNiagaraParameterHandle InputHandle(InputVariable.GetName());
    const FNiagaraParameterHandle ModuleHandle = InputHandle.IsModuleHandle()
        ? InputHandle
        : FNiagaraParameterHandle::CreateModuleParameterHandle(InputVariable.GetName());
    const FNiagaraParameterHandle AliasedHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleHandle, FunctionCall);
    FNiagaraVariable AliasedVariable(InputVariable.GetType(), AliasedHandle.GetParameterHandleString());
    return FNiagaraUtilities::ConvertVariableToRapidIterationConstantName(AliasedVariable, *EmitterName, ScriptUsage);
}

TSharedPtr<FJsonObject> BuildResolvedStackInputJson(
    const UNiagaraNodeFunctionCall* FunctionCall,
    const FNiagaraVariable& InputVariable,
    const TSet<FNiagaraVariable>& HiddenVariables,
    UNiagaraScript* OwningScript,
    const FString& EmitterName)
{
    TSharedPtr<FJsonObject> InputObject = MakeShared<FJsonObject>();
    InputObject->SetObjectField(TEXT("variable"), NiagaraVariableToJsonObject(InputVariable));
    InputObject->SetBoolField(TEXT("is_hidden"), HiddenVariables.Contains(InputVariable));
    InputObject->SetBoolField(TEXT("can_author_now"), false);

    if (!OwningScript)
    {
        InputObject->SetStringField(TEXT("value_source"), TEXT("no_owning_script"));
        return InputObject;
    }

    FNiagaraVariable RapidIterationParameter = BuildRapidIterationParameterForInput(
        FunctionCall,
        InputVariable,
        EmitterName,
        OwningScript->GetUsage());
    const uint8* ValueData = RapidIterationParameter.IsValid()
        ? OwningScript->RapidIterationParameters.GetParameterData(RapidIterationParameter)
        : nullptr;

    TSharedPtr<FJsonObject> RapidIterationObject = NiagaraVariableToJsonObject(RapidIterationParameter);
    RapidIterationObject->SetBoolField(TEXT("has_value"), ValueData != nullptr);
    if (ValueData)
    {
        RapidIterationParameter.SetData(ValueData);
        RapidIterationObject->SetField(TEXT("value"), NiagaraVariableDataToJsonValue(RapidIterationParameter));
        RapidIterationObject->SetStringField(TEXT("value_string"), RapidIterationParameter.ToString());
        InputObject->SetStringField(TEXT("value_source"), TEXT("rapid_iteration"));
    }
    else
    {
        RapidIterationObject->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
        InputObject->SetStringField(TEXT("value_source"), TEXT("unresolved_default"));
    }
    InputObject->SetObjectField(TEXT("rapid_iteration_parameter"), RapidIterationObject);
    return InputObject;
}

TArray<TSharedPtr<FJsonValue>> BuildResolvedStackInputsJson(
    const UNiagaraNodeFunctionCall* FunctionCall,
    FVersionedNiagaraEmitter OwningEmitter,
    FVersionedNiagaraEmitterData* EmitterData,
    const FString& EmitterName,
    int32 MaxResolvedInputs)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    if (!FunctionCall)
    {
        return Values;
    }

    const ENiagaraScriptUsage OutputUsage = FNiagaraStackGraphUtilities::GetOutputNodeUsage(*FunctionCall);
    FCompileConstantResolver ConstantResolver(OwningEmitter, OutputUsage, FunctionCall->DebugState);

    TArray<FNiagaraVariable> InputVariables;
    TSet<FNiagaraVariable> HiddenVariables;
    FNiagaraStackGraphUtilities::GetStackFunctionInputs(
        *FunctionCall,
        InputVariables,
        HiddenVariables,
        ConstantResolver,
        FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly,
        true);

    UNiagaraScript* OwningScript = FindEmitterScriptForUsage(EmitterData, OutputUsage);
    const int32 SafeMaxInputs = FMath::Max(0, MaxResolvedInputs);
    for (int32 Index = 0; Index < InputVariables.Num() && Index < SafeMaxInputs; ++Index)
    {
        Values.Add(MakeShared<FJsonValueObject>(BuildResolvedStackInputJson(
            FunctionCall,
            InputVariables[Index],
            HiddenVariables,
            OwningScript,
            EmitterName)));
    }
    return Values;
}

TSharedPtr<FJsonObject> BuildModuleInputModuleJson(
    const UNiagaraNodeFunctionCall* FunctionCall,
    FVersionedNiagaraEmitter OwningEmitter,
    FVersionedNiagaraEmitterData* EmitterData,
    const FString& EmitterName,
    int32 EmitterIndex,
    int32 ModuleIndex,
    bool bIncludeLinkedSources,
    bool bIncludeResolvedStackInputs,
    int32 MaxCandidatesPerModule,
    int32 MaxResolvedInputsPerModule,
    int32& OutCandidateCount,
    TArray<TSharedPtr<FJsonObject>>& OutTopCandidates)
{
    TSharedPtr<FJsonObject> ModuleObject = BuildFunctionCallJson(FunctionCall, ModuleIndex, false);
    TArray<TSharedPtr<FJsonValue>> CandidateValues;
    if (!FunctionCall)
    {
        ModuleObject->SetArrayField(TEXT("input_candidates"), CandidateValues);
        ModuleObject->SetNumberField(TEXT("input_candidate_count"), 0);
        return ModuleObject;
    }

    const TArray<TSharedPtr<FJsonValue>> ResolvedStackInputValues = bIncludeResolvedStackInputs
        ? BuildResolvedStackInputsJson(FunctionCall, OwningEmitter, EmitterData, EmitterName, MaxResolvedInputsPerModule)
        : TArray<TSharedPtr<FJsonValue>>();
    ModuleObject->SetBoolField(TEXT("resolved_stack_inputs_enabled"), bIncludeResolvedStackInputs);
    ModuleObject->SetNumberField(TEXT("resolved_stack_input_count"), ResolvedStackInputValues.Num());
    ModuleObject->SetArrayField(TEXT("resolved_stack_inputs"), ResolvedStackInputValues);

    TArray<UEdGraphPin*> InputPins;
    FunctionCall->GetInputPins(InputPins);
    int32 CandidateCount = 0;
    for (const UEdGraphPin* Pin : InputPins)
    {
        if (!IsCandidateNiagaraInputPin(Pin))
        {
            continue;
        }

        TSharedPtr<FJsonObject> CandidateObject = BuildModuleInputCandidateJson(
            FunctionCall,
            Pin,
            EmitterName,
            EmitterIndex,
            ModuleIndex,
            bIncludeLinkedSources);
        ++CandidateCount;
        ++OutCandidateCount;
        OutTopCandidates.Add(CandidateObject);
        if (CandidateValues.Num() < MaxCandidatesPerModule)
        {
            CandidateValues.Add(MakeShared<FJsonValueObject>(CandidateObject));
        }
    }

    ModuleObject->SetNumberField(TEXT("input_candidate_count"), CandidateCount);
    ModuleObject->SetArrayField(TEXT("input_candidates"), CandidateValues);
    return ModuleObject;
}

TArray<UNiagaraNodeFunctionCall*> CollectSortedNiagaraFunctionCalls(const UNiagaraScriptSourceBase* SourceBase)
{
    TArray<UNiagaraNodeFunctionCall*> FunctionCalls;
    const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(SourceBase);
    UNiagaraGraph* Graph = Source ? Source->NodeGraph : nullptr;
    if (!Graph)
    {
        return FunctionCalls;
    }

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(Node))
        {
            FunctionCalls.Add(FunctionCall);
        }
    }

    FunctionCalls.Sort(
        [](const UNiagaraNodeFunctionCall& Left, const UNiagaraNodeFunctionCall& Right)
        {
            if (Left.NodePosY == Right.NodePosY)
            {
                return Left.NodePosX < Right.NodePosX;
            }
            return Left.NodePosY < Right.NodePosY;
        });
    return FunctionCalls;
}

TSharedPtr<FJsonObject> BuildFunctionCallJson(const UNiagaraNodeFunctionCall* FunctionCall, int32 NodeIndex, bool bIncludePins)
{
    TSharedPtr<FJsonObject> NodeObject = MakeShared<FJsonObject>();
    if (!FunctionCall)
    {
        return NodeObject;
    }

    FString FunctionName = FunctionCall->GetFunctionName();
    if (FunctionName.IsEmpty())
    {
        FunctionName = FunctionCall->Signature.GetNameString();
    }

    const UNiagaraScript* FunctionScript = FunctionCall->FunctionScript;
    const UObject* ScriptOuter = FunctionScript ? FunctionScript->GetOuter() : nullptr;
    const bool bIsScratchPad = ScriptOuter && ScriptOuter->IsA<UNiagaraScratchPadContainer>();

    NodeObject->SetNumberField(TEXT("node_index"), NodeIndex);
    NodeObject->SetStringField(TEXT("node_name"), FunctionCall->GetName());
    NodeObject->SetStringField(TEXT("node_guid"), FunctionCall->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
    NodeObject->SetStringField(TEXT("function_name"), FunctionName);
    NodeObject->SetStringField(TEXT("function_script"), NiagaraObjectPathOrEmpty(FunctionScript));
    NodeObject->SetStringField(TEXT("function_script_asset_object_path"), FunctionCall->FunctionScriptAssetObjectPath.ToString());
    NodeObject->SetStringField(TEXT("script_usage"), FunctionScript ? NiagaraScriptUsageName(FunctionScript->GetUsage()) : FString());
    NodeObject->SetStringField(TEXT("called_usage"), NiagaraScriptUsageName(FunctionCall->GetCalledUsage()));
    NodeObject->SetBoolField(TEXT("is_scratch_pad"), bIsScratchPad);
    NodeObject->SetStringField(TEXT("source_kind"), bIsScratchPad ? TEXT("scratch_pad") : (FunctionScript ? TEXT("script_asset") : TEXT("signature_only")));
    NodeObject->SetArrayField(TEXT("signature_inputs"), NiagaraVariablesToJson(FunctionCall->Signature.Inputs));
    NodeObject->SetArrayField(TEXT("signature_outputs"), NiagaraVariableBasesToJson(FunctionCall->Signature.Outputs));

    TArray<FString> SpecifierValues;
    for (const TPair<FName, FName>& Specifier : FunctionCall->FunctionSpecifiers)
    {
        AddUniqueString(SpecifierValues, FString::Printf(TEXT("%s=%s"), *Specifier.Key.ToString(), *Specifier.Value.ToString()));
    }
    NodeObject->SetArrayField(TEXT("function_specifiers"), StringsToJson(SpecifierValues));

    FString SearchText = FunctionName;
    SearchText += TEXT(" ");
    SearchText += NiagaraObjectPathOrEmpty(FunctionScript);
    for (const FNiagaraVariable& Variable : FunctionCall->Signature.Inputs)
    {
        SearchText += TEXT(" ");
        SearchText += Variable.GetName().ToString();
    }
    for (const FNiagaraVariable& Variable : FunctionCall->Signature.Outputs)
    {
        SearchText += TEXT(" ");
        SearchText += Variable.GetName().ToString();
    }
    NodeObject->SetArrayField(TEXT("control_hints"), StringsToJson(BuildNiagaraControlHints(SearchText)));

    if (bIncludePins)
    {
        TArray<UEdGraphPin*> InputPins;
        TArray<UEdGraphPin*> OutputPins;
        FunctionCall->GetInputPins(InputPins);
        FunctionCall->GetOutputPins(OutputPins);
        NodeObject->SetArrayField(TEXT("input_pins"), GraphPinsToJson(InputPins));
        NodeObject->SetArrayField(TEXT("output_pins"), GraphPinsToJson(OutputPins));
    }

    return NodeObject;
}

TSharedPtr<FJsonObject> BuildInputNodeJson(const UNiagaraNodeInput* InputNode)
{
    TSharedPtr<FJsonObject> InputObject = MakeShared<FJsonObject>();
    if (!InputNode)
    {
        return InputObject;
    }

    InputObject->SetStringField(TEXT("node_name"), InputNode->GetName());
    InputObject->SetStringField(TEXT("node_guid"), InputNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
    InputObject->SetObjectField(TEXT("variable"), NiagaraVariableToJsonObject(InputNode->Input));
    InputObject->SetNumberField(TEXT("call_sort_priority"), InputNode->CallSortPriority);
    InputObject->SetBoolField(TEXT("is_exposed"), InputNode->IsExposed());
    InputObject->SetBoolField(TEXT("is_required"), InputNode->IsRequired());
    InputObject->SetBoolField(TEXT("is_hidden"), InputNode->IsHidden());
    InputObject->SetBoolField(TEXT("can_auto_bind"), InputNode->CanAutoBind());
    InputObject->SetArrayField(TEXT("control_hints"), StringsToJson(BuildNiagaraControlHints(InputNode->Input.GetName().ToString())));
    return InputObject;
}

TSharedPtr<FJsonObject> BuildOutputNodeJson(const UNiagaraNodeOutput* OutputNode)
{
    TSharedPtr<FJsonObject> OutputObject = MakeShared<FJsonObject>();
    if (!OutputNode)
    {
        return OutputObject;
    }

    OutputObject->SetStringField(TEXT("node_name"), OutputNode->GetName());
    OutputObject->SetStringField(TEXT("node_guid"), OutputNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
    OutputObject->SetStringField(TEXT("usage"), NiagaraScriptUsageName(OutputNode->GetUsage()));
    OutputObject->SetStringField(TEXT("usage_id"), OutputNode->GetUsageId().ToString(EGuidFormats::DigitsWithHyphens));
    OutputObject->SetArrayField(TEXT("outputs"), NiagaraVariablesToJson(OutputNode->GetOutputs()));
    return OutputObject;
}

TSharedPtr<FJsonObject> BuildGraphAnalysisJson(
    const UNiagaraScriptSourceBase* SourceBase,
    const FString& Context,
    bool bIncludePins,
    int32 MaxFunctionCalls)
{
    TSharedPtr<FJsonObject> GraphObject = MakeShared<FJsonObject>();
    GraphObject->SetStringField(TEXT("context"), Context);
    GraphObject->SetStringField(TEXT("source_class"), SourceBase ? SourceBase->GetClass()->GetName() : FString());
    GraphObject->SetStringField(TEXT("source_path"), NiagaraObjectPathOrEmpty(SourceBase));

    const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(SourceBase);
    UNiagaraGraph* Graph = Source ? Source->NodeGraph : nullptr;
    GraphObject->SetStringField(TEXT("graph_path"), NiagaraObjectPathOrEmpty(Graph));
    GraphObject->SetBoolField(TEXT("has_graph"), Graph != nullptr);

    if (!Graph)
    {
        GraphObject->SetNumberField(TEXT("node_count"), 0);
        GraphObject->SetNumberField(TEXT("function_call_count"), 0);
        GraphObject->SetNumberField(TEXT("input_node_count"), 0);
        GraphObject->SetNumberField(TEXT("output_node_count"), 0);
        GraphObject->SetArrayField(TEXT("function_calls"), TArray<TSharedPtr<FJsonValue>>());
        GraphObject->SetArrayField(TEXT("input_nodes"), TArray<TSharedPtr<FJsonValue>>());
        GraphObject->SetArrayField(TEXT("output_nodes"), TArray<TSharedPtr<FJsonValue>>());
        return GraphObject;
    }

    TArray<UNiagaraNodeFunctionCall*> FunctionCalls;
    TArray<UNiagaraNodeInput*> InputNodes;
    TArray<UNiagaraNodeOutput*> OutputNodes;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(Node))
        {
            FunctionCalls.Add(FunctionCall);
        }
        else if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node))
        {
            InputNodes.Add(InputNode);
        }
        else if (UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(Node))
        {
            OutputNodes.Add(OutputNode);
        }
    }

    FunctionCalls.Sort(
        [](const UNiagaraNodeFunctionCall& Left, const UNiagaraNodeFunctionCall& Right)
        {
            if (Left.NodePosY == Right.NodePosY)
            {
                return Left.NodePosX < Right.NodePosX;
            }
            return Left.NodePosY < Right.NodePosY;
        });
    InputNodes.Sort(
        [](const UNiagaraNodeInput& Left, const UNiagaraNodeInput& Right)
        {
            return Left.CallSortPriority < Right.CallSortPriority;
        });

    TArray<TSharedPtr<FJsonValue>> FunctionValues;
    TArray<FString> ControlHints;
    const int32 SafeMaxFunctionCalls = FMath::Max(0, MaxFunctionCalls);
    for (int32 Index = 0; Index < FunctionCalls.Num() && Index < SafeMaxFunctionCalls; ++Index)
    {
        TSharedPtr<FJsonObject> FunctionObject = BuildFunctionCallJson(FunctionCalls[Index], Index, bIncludePins);
        for (const TSharedPtr<FJsonValue>& HintValue : FunctionObject->GetArrayField(TEXT("control_hints")))
        {
            FString Hint;
            if (HintValue->TryGetString(Hint))
            {
                AddUniqueString(ControlHints, Hint);
            }
        }
        FunctionValues.Add(MakeShared<FJsonValueObject>(FunctionObject));
    }

    TArray<TSharedPtr<FJsonValue>> InputValues;
    for (const UNiagaraNodeInput* InputNode : InputNodes)
    {
        TSharedPtr<FJsonObject> InputObject = BuildInputNodeJson(InputNode);
        for (const TSharedPtr<FJsonValue>& HintValue : InputObject->GetArrayField(TEXT("control_hints")))
        {
            FString Hint;
            if (HintValue->TryGetString(Hint))
            {
                AddUniqueString(ControlHints, Hint);
            }
        }
        InputValues.Add(MakeShared<FJsonValueObject>(InputObject));
    }

    TArray<TSharedPtr<FJsonValue>> OutputValues;
    for (const UNiagaraNodeOutput* OutputNode : OutputNodes)
    {
        OutputValues.Add(MakeShared<FJsonValueObject>(BuildOutputNodeJson(OutputNode)));
    }

    GraphObject->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
    GraphObject->SetNumberField(TEXT("function_call_count"), FunctionCalls.Num());
    GraphObject->SetNumberField(TEXT("input_node_count"), InputNodes.Num());
    GraphObject->SetNumberField(TEXT("output_node_count"), OutputNodes.Num());
    GraphObject->SetBoolField(TEXT("function_calls_truncated"), FunctionCalls.Num() > SafeMaxFunctionCalls);
    GraphObject->SetArrayField(TEXT("control_hints"), StringsToJson(ControlHints));
    GraphObject->SetArrayField(TEXT("function_calls"), FunctionValues);
    GraphObject->SetArrayField(TEXT("input_nodes"), InputValues);
    GraphObject->SetArrayField(TEXT("output_nodes"), OutputValues);
    return GraphObject;
}

TSharedPtr<FJsonObject> BuildScratchPadScriptJson(
    const UNiagaraScript* Script,
    const FString& OwnerKind,
    bool bIncludePins,
    int32 MaxFunctionCalls)
{
    TSharedPtr<FJsonObject> ScriptObject = MakeShared<FJsonObject>();
    if (!Script)
    {
        return ScriptObject;
    }

    ScriptObject->SetStringField(TEXT("name"), Script->GetName());
    ScriptObject->SetStringField(TEXT("path"), Script->GetPathName());
    ScriptObject->SetStringField(TEXT("owner_kind"), OwnerKind);
    ScriptObject->SetStringField(TEXT("usage"), NiagaraScriptUsageName(Script->GetUsage()));
    TArray<ENiagaraScriptUsage> SupportedUsageContexts;
    if (const FVersionedNiagaraScriptData* ScriptData = Script->GetLatestScriptData())
    {
        SupportedUsageContexts = ScriptData->GetSupportedUsageContexts();
    }
    ScriptObject->SetArrayField(TEXT("supported_usage_contexts"), NiagaraScriptUsagesToJson(SupportedUsageContexts));
    ScriptObject->SetObjectField(
        TEXT("graph"),
        BuildGraphAnalysisJson(Script->GetLatestSource(), FString::Printf(TEXT("%s_scratch_pad:%s"), *OwnerKind, *Script->GetName()), bIncludePins, MaxFunctionCalls));
    return ScriptObject;
}

void AppendScratchPadContainerScripts(
    const UNiagaraScratchPadContainer* Container,
    const FString& OwnerKind,
    bool bIncludePins,
    int32 MaxFunctionCalls,
    TArray<TSharedPtr<FJsonValue>>& OutScripts)
{
#if WITH_EDITORONLY_DATA
    if (!Container)
    {
        return;
    }

    for (const TObjectPtr<UNiagaraScript>& Script : Container->Scripts)
    {
        OutScripts.Add(MakeShared<FJsonValueObject>(BuildScratchPadScriptJson(Script.Get(), OwnerKind, bIncludePins, MaxFunctionCalls)));
    }
#endif
}
}

FUnrealMCPNiagaraCommands::FUnrealMCPNiagaraCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("inspect_niagara_renderers"))
    {
        return HandleInspectNiagaraRenderers(Params);
    }
    if (CommandType == TEXT("set_niagara_renderer_material"))
    {
        return HandleSetNiagaraRendererMaterial(Params);
    }
    if (CommandType == TEXT("inspect_niagara_user_parameters"))
    {
        return HandleInspectNiagaraUserParameters(Params);
    }
    if (CommandType == TEXT("set_niagara_user_parameter"))
    {
        return HandleSetNiagaraUserParameter(Params);
    }
    if (CommandType == TEXT("inspect_niagara_stack"))
    {
        return HandleInspectNiagaraStack(Params);
    }
    if (CommandType == TEXT("inspect_niagara_module_inputs"))
    {
        return HandleInspectNiagaraModuleInputs(Params);
    }
    if (CommandType == TEXT("set_niagara_module_input_value"))
    {
        return HandleSetNiagaraModuleInputValue(Params);
    }

    return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown Niagara command: %s"), *CommandType));
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::BuildRendererJson(
    const UNiagaraSystem* System,
    const FString& EmitterName,
    int32 EmitterIndex,
    const UNiagaraRendererProperties* Renderer,
    int32 RendererIndex) const
{
    TSharedPtr<FJsonObject> RendererObject = MakeShared<FJsonObject>();
    RendererObject->SetStringField(TEXT("system_path"), System ? System->GetPathName() : FString());
    RendererObject->SetStringField(TEXT("emitter_name"), EmitterName);
    RendererObject->SetNumberField(TEXT("emitter_index"), EmitterIndex);
    RendererObject->SetNumberField(TEXT("renderer_index"), RendererIndex);
    RendererObject->SetStringField(TEXT("renderer_class"), Renderer ? Renderer->GetClass()->GetName() : FString());
    RendererObject->SetStringField(TEXT("renderer_name"), Renderer ? Renderer->GetName() : FString());
    RendererObject->SetBoolField(TEXT("enabled"), Renderer ? Renderer->GetIsEnabled() : false);
    RendererObject->SetStringField(TEXT("material_field"), Renderer ? RendererMaterialField(Renderer) : FString());

    TArray<UMaterialInterface*> UsedMaterials;
    if (Renderer)
    {
        Renderer->GetUsedMaterials(nullptr, UsedMaterials);
    }
    RendererObject->SetArrayField(TEXT("used_materials"), MaterialArrayToJson(UsedMaterials));

    if (const UNiagaraSpriteRendererProperties* SpriteRenderer = Cast<UNiagaraSpriteRendererProperties>(Renderer))
    {
        RendererObject->SetStringField(TEXT("primary_material"), NiagaraObjectPathOrEmpty(SpriteRenderer->Material));
    }
    else if (const UNiagaraRibbonRendererProperties* RibbonRenderer = Cast<UNiagaraRibbonRendererProperties>(Renderer))
    {
        RendererObject->SetStringField(TEXT("primary_material"), NiagaraObjectPathOrEmpty(RibbonRenderer->Material));
    }
    else if (const UNiagaraMeshRendererProperties* MeshRenderer = Cast<UNiagaraMeshRendererProperties>(Renderer))
    {
        TArray<TSharedPtr<FJsonValue>> OverrideValues;
        for (int32 OverrideIndex = 0; OverrideIndex < MeshRenderer->OverrideMaterials.Num(); ++OverrideIndex)
        {
            TSharedPtr<FJsonObject> OverrideObject = MakeShared<FJsonObject>();
            OverrideObject->SetNumberField(TEXT("slot_index"), OverrideIndex);
            OverrideObject->SetStringField(TEXT("explicit_material"), NiagaraObjectPathOrEmpty(MeshRenderer->OverrideMaterials[OverrideIndex].ExplicitMat));
            OverrideValues.Add(MakeShared<FJsonValueObject>(OverrideObject));
        }
        RendererObject->SetBoolField(TEXT("override_materials_enabled"), MeshRenderer->bOverrideMaterials != 0);
        RendererObject->SetArrayField(TEXT("override_materials"), OverrideValues);
    }
    else if (const UNiagaraDecalRendererProperties* DecalRenderer = Cast<UNiagaraDecalRendererProperties>(Renderer))
    {
        RendererObject->SetStringField(TEXT("primary_material"), NiagaraObjectPathOrEmpty(DecalRenderer->Material));
    }
    else if (const UNiagaraVolumeRendererProperties* VolumeRenderer = Cast<UNiagaraVolumeRendererProperties>(Renderer))
    {
        RendererObject->SetStringField(TEXT("primary_material"), NiagaraObjectPathOrEmpty(VolumeRenderer->Material));
    }

    return RendererObject;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::BuildUserParameterJson(const UNiagaraSystem* System, const FNiagaraVariable& Parameter) const
{
    const FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
    TSharedPtr<FJsonObject> ParameterObject = MakeShared<FJsonObject>();
    ParameterObject->SetStringField(TEXT("name"), Parameter.GetName().ToString());
    ParameterObject->SetStringField(TEXT("type"), NiagaraTypeName(Parameter.GetType()));
    ParameterObject->SetNumberField(TEXT("size_bytes"), Parameter.GetSizeInBytes());
    ParameterObject->SetBoolField(TEXT("settable_by_mcp"), IsSettableNiagaraUserParameterType(Parameter.GetType()));
    ParameterObject->SetField(TEXT("value"), NiagaraParameterValueToJson(Store, Parameter));
    return ParameterObject;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleInspectNiagaraRenderers(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    TArray<TSharedPtr<FJsonValue>> RendererValues;
    int32 EmitterIndex = 0;
    for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
    {
        const FString EmitterName = EmitterHandle.GetName().ToString();
        EmitterHandle.ForEachEnabledRendererWithIndex(
            [this, System, EmitterName, EmitterIndex, &RendererValues](const UNiagaraRendererProperties* Renderer, int32 RendererIndex)
            {
                RendererValues.Add(MakeShared<FJsonValueObject>(BuildRendererJson(System, EmitterName, EmitterIndex, Renderer, RendererIndex)));
            });
        ++EmitterIndex;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetNumberField(TEXT("emitter_count"), System->GetEmitterHandles().Num());
    Result->SetNumberField(TEXT("renderer_count"), RendererValues.Num());
    Result->SetArrayField(TEXT("renderers"), RendererValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleSetNiagaraRendererMaterial(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString MaterialPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'material_path' parameter"));
    }

    bool bAllowSourceEdit = false;
    Params->TryGetBoolField(TEXT("allow_source_edit"), bAllowSourceEdit);
    if (!bAllowSourceEdit && !IsTempGeneratedNiagaraPath(SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Refusing to edit Niagara system outside %s: %s"), NiagaraTempGenerationRoot, *SystemPath));
    }

    int32 TargetEmitterIndex = INDEX_NONE;
    int32 TargetRendererIndex = INDEX_NONE;
    int32 MaterialSlotIndex = 0;
    FString TargetEmitterName;
    Params->TryGetNumberField(TEXT("emitter_index"), TargetEmitterIndex);
    Params->TryGetNumberField(TEXT("renderer_index"), TargetRendererIndex);
    Params->TryGetNumberField(TEXT("material_slot_index"), MaterialSlotIndex);
    Params->TryGetStringField(TEXT("emitter_name"), TargetEmitterName);

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *NormalizeNiagaraObjectPathForLoad(MaterialPath));
    if (!Material)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load material: %s"), *MaterialPath));
    }

    UNiagaraRendererProperties* MatchedRenderer = nullptr;
    int32 MatchedEmitterIndex = INDEX_NONE;
    int32 MatchedRendererIndex = INDEX_NONE;
    FString MatchedEmitterName;

    int32 CurrentEmitterIndex = 0;
    for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
    {
        const FString EmitterName = EmitterHandle.GetName().ToString();
        const bool bEmitterMatches =
            (TargetEmitterIndex == INDEX_NONE || TargetEmitterIndex == CurrentEmitterIndex) &&
            (TargetEmitterName.IsEmpty() || TargetEmitterName.Equals(EmitterName, ESearchCase::IgnoreCase));

        if (bEmitterMatches)
        {
            if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
            {
                EmitterData->ForEachEnabledRendererWithIndex(
                    [&MatchedRenderer, &MatchedEmitterIndex, &MatchedRendererIndex, &MatchedEmitterName, CurrentEmitterIndex, EmitterName, TargetRendererIndex](UNiagaraRendererProperties* Renderer, int32 RendererIndex)
                    {
                        if (MatchedRenderer == nullptr && (TargetRendererIndex == INDEX_NONE || TargetRendererIndex == RendererIndex))
                        {
                            MatchedRenderer = Renderer;
                            MatchedEmitterIndex = CurrentEmitterIndex;
                            MatchedRendererIndex = RendererIndex;
                            MatchedEmitterName = EmitterName;
                        }
                    });
            }
        }

        ++CurrentEmitterIndex;
    }

    if (!MatchedRenderer)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No matching enabled Niagara renderer was found"));
    }

    FString ChangedField;
    FString ErrorMessage;
    if (!SetRendererMaterial(MatchedRenderer, Material, MaterialSlotIndex, ChangedField, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    System->Modify();
    System->MarkPackageDirty();
    System->RequestCompile(false);

    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveLoadedAsset(System, false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetStringField(TEXT("material_path"), Material->GetPathName());
    Result->SetStringField(TEXT("emitter_name"), MatchedEmitterName);
    Result->SetNumberField(TEXT("emitter_index"), MatchedEmitterIndex);
    Result->SetNumberField(TEXT("renderer_index"), MatchedRendererIndex);
    Result->SetStringField(TEXT("renderer_class"), MatchedRenderer->GetClass()->GetName());
    Result->SetStringField(TEXT("changed_field"), ChangedField);
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleInspectNiagaraUserParameters(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    TArray<FNiagaraVariable> UserParameters;
    System->GetExposedParameters().GetUserParameters(UserParameters);

    TArray<TSharedPtr<FJsonValue>> ParameterValues;
    int32 SettableCount = 0;
    for (const FNiagaraVariable& Parameter : UserParameters)
    {
        if (IsSettableNiagaraUserParameterType(Parameter.GetType()))
        {
            ++SettableCount;
        }
        ParameterValues.Add(MakeShared<FJsonValueObject>(BuildUserParameterJson(System, Parameter)));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetNumberField(TEXT("parameter_count"), ParameterValues.Num());
    Result->SetNumberField(TEXT("settable_count"), SettableCount);
    Result->SetArrayField(TEXT("parameters"), ParameterValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleInspectNiagaraStack(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }

    bool bIncludePins = false;
    Params->TryGetBoolField(TEXT("include_pins"), bIncludePins);

    int32 MaxFunctionCalls = 200;
    Params->TryGetNumberField(TEXT("max_function_calls"), MaxFunctionCalls);
    MaxFunctionCalls = FMath::Clamp(MaxFunctionCalls, 0, 1000);

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    TArray<TSharedPtr<FJsonValue>> SystemScriptValues;
    if (const UNiagaraScript* SystemSpawnScript = System->GetSystemSpawnScript())
    {
        TSharedPtr<FJsonObject> ScriptObject = MakeShared<FJsonObject>();
        ScriptObject->SetStringField(TEXT("name"), SystemSpawnScript->GetName());
        ScriptObject->SetStringField(TEXT("path"), SystemSpawnScript->GetPathName());
        ScriptObject->SetStringField(TEXT("usage"), NiagaraScriptUsageName(SystemSpawnScript->GetUsage()));
        ScriptObject->SetObjectField(TEXT("graph"), BuildGraphAnalysisJson(SystemSpawnScript->GetLatestSource(), TEXT("system_spawn"), bIncludePins, MaxFunctionCalls));
        SystemScriptValues.Add(MakeShared<FJsonValueObject>(ScriptObject));
    }
    if (const UNiagaraScript* SystemUpdateScript = System->GetSystemUpdateScript())
    {
        TSharedPtr<FJsonObject> ScriptObject = MakeShared<FJsonObject>();
        ScriptObject->SetStringField(TEXT("name"), SystemUpdateScript->GetName());
        ScriptObject->SetStringField(TEXT("path"), SystemUpdateScript->GetPathName());
        ScriptObject->SetStringField(TEXT("usage"), NiagaraScriptUsageName(SystemUpdateScript->GetUsage()));
        ScriptObject->SetObjectField(TEXT("graph"), BuildGraphAnalysisJson(SystemUpdateScript->GetLatestSource(), TEXT("system_update"), bIncludePins, MaxFunctionCalls));
        SystemScriptValues.Add(MakeShared<FJsonValueObject>(ScriptObject));
    }

    TArray<TSharedPtr<FJsonValue>> SystemScratchPadValues;
#if WITH_EDITORONLY_DATA
    for (const TObjectPtr<UNiagaraScript>& ScratchPadScript : System->ScratchPadScripts)
    {
        SystemScratchPadValues.Add(MakeShared<FJsonValueObject>(BuildScratchPadScriptJson(ScratchPadScript.Get(), TEXT("system"), bIncludePins, MaxFunctionCalls)));
    }
#endif

    TArray<TSharedPtr<FJsonValue>> EmitterValues;
    int32 TotalFunctionCallCount = 0;
    int32 TotalScratchPadCount = SystemScratchPadValues.Num();
    int32 EmitterIndex = 0;
    for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
    {
        TSharedPtr<FJsonObject> EmitterObject = MakeShared<FJsonObject>();
        EmitterObject->SetStringField(TEXT("name"), EmitterHandle.GetName().ToString());
        EmitterObject->SetNumberField(TEXT("emitter_index"), EmitterIndex);
        EmitterObject->SetBoolField(TEXT("enabled"), EmitterHandle.GetIsEnabled());

        TArray<TSharedPtr<FJsonValue>> ScriptValues;
        TArray<TSharedPtr<FJsonValue>> ScratchPadValues;
        TArray<TSharedPtr<FJsonValue>> ParentScratchPadValues;

        if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
        {
            TSharedPtr<FJsonObject> GraphObject = BuildGraphAnalysisJson(
                EmitterData->GraphSource,
                FString::Printf(TEXT("emitter:%s"), *EmitterHandle.GetName().ToString()),
                bIncludePins,
                MaxFunctionCalls);
            EmitterObject->SetObjectField(TEXT("graph"), GraphObject);
            TotalFunctionCallCount += static_cast<int32>(GraphObject->GetNumberField(TEXT("function_call_count")));

            TArray<UNiagaraScript*> EmitterScripts;
            EmitterData->GetScripts(EmitterScripts, false, false);
            for (const UNiagaraScript* Script : EmitterScripts)
            {
                if (!Script)
                {
                    continue;
                }
                TSharedPtr<FJsonObject> ScriptObject = MakeShared<FJsonObject>();
                ScriptObject->SetStringField(TEXT("name"), Script->GetName());
                ScriptObject->SetStringField(TEXT("path"), Script->GetPathName());
                ScriptObject->SetStringField(TEXT("usage"), NiagaraScriptUsageName(Script->GetUsage()));
                ScriptValues.Add(MakeShared<FJsonValueObject>(ScriptObject));
            }

#if WITH_EDITORONLY_DATA
            AppendScratchPadContainerScripts(EmitterData->ScratchPads, TEXT("emitter"), bIncludePins, MaxFunctionCalls, ScratchPadValues);
            AppendScratchPadContainerScripts(EmitterData->ParentScratchPads, TEXT("parent_emitter"), bIncludePins, MaxFunctionCalls, ParentScratchPadValues);
#endif
        }
        else
        {
            EmitterObject->SetObjectField(TEXT("graph"), BuildGraphAnalysisJson(nullptr, FString::Printf(TEXT("emitter:%s"), *EmitterHandle.GetName().ToString()), bIncludePins, MaxFunctionCalls));
        }

        TotalScratchPadCount += ScratchPadValues.Num() + ParentScratchPadValues.Num();
        EmitterObject->SetArrayField(TEXT("scripts"), ScriptValues);
        EmitterObject->SetArrayField(TEXT("scratch_pad_scripts"), ScratchPadValues);
        EmitterObject->SetArrayField(TEXT("parent_scratch_pad_scripts"), ParentScratchPadValues);
        EmitterValues.Add(MakeShared<FJsonValueObject>(EmitterObject));
        ++EmitterIndex;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetBoolField(TEXT("include_pins"), bIncludePins);
    Result->SetNumberField(TEXT("max_function_calls"), MaxFunctionCalls);
    Result->SetNumberField(TEXT("emitter_count"), System->GetEmitterHandles().Num());
    Result->SetNumberField(TEXT("system_script_count"), SystemScriptValues.Num());
    Result->SetNumberField(TEXT("system_scratch_pad_count"), SystemScratchPadValues.Num());
    Result->SetNumberField(TEXT("total_scratch_pad_count"), TotalScratchPadCount);
    Result->SetNumberField(TEXT("total_emitter_function_call_count"), TotalFunctionCallCount);
    Result->SetArrayField(TEXT("system_scripts"), SystemScriptValues);
    Result->SetArrayField(TEXT("system_scratch_pad_scripts"), SystemScratchPadValues);
    Result->SetArrayField(TEXT("emitters"), EmitterValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleInspectNiagaraModuleInputs(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }

    bool bIncludeLinkedSources = true;
    Params->TryGetBoolField(TEXT("include_linked_sources"), bIncludeLinkedSources);

    bool bIncludeResolvedStackInputs = false;
    Params->TryGetBoolField(TEXT("include_resolved_stack_inputs"), bIncludeResolvedStackInputs);

    int32 MaxModules = 200;
    Params->TryGetNumberField(TEXT("max_modules"), MaxModules);
    MaxModules = FMath::Clamp(MaxModules, 0, 1000);

    int32 MaxCandidatesPerModule = 24;
    Params->TryGetNumberField(TEXT("max_candidates_per_module"), MaxCandidatesPerModule);
    MaxCandidatesPerModule = FMath::Clamp(MaxCandidatesPerModule, 0, 200);

    int32 MaxResolvedInputsPerModule = 8;
    Params->TryGetNumberField(TEXT("max_resolved_inputs_per_module"), MaxResolvedInputsPerModule);
    MaxResolvedInputsPerModule = FMath::Clamp(MaxResolvedInputsPerModule, 0, 200);

    int32 MaxTopCandidates = 80;
    Params->TryGetNumberField(TEXT("max_top_candidates"), MaxTopCandidates);
    MaxTopCandidates = FMath::Clamp(MaxTopCandidates, 0, 500);

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    TArray<TSharedPtr<FJsonValue>> EmitterValues;
    TArray<TSharedPtr<FJsonObject>> TopCandidateObjects;
    int32 TotalModuleCount = 0;
    int32 TotalCandidateCount = 0;

    int32 EmitterIndex = 0;
    for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
    {
        TSharedPtr<FJsonObject> EmitterObject = MakeShared<FJsonObject>();
        const FString EmitterName = EmitterHandle.GetName().ToString();
        EmitterObject->SetStringField(TEXT("name"), EmitterName);
        EmitterObject->SetNumberField(TEXT("emitter_index"), EmitterIndex);
        EmitterObject->SetBoolField(TEXT("enabled"), EmitterHandle.GetIsEnabled());

        TArray<TSharedPtr<FJsonValue>> ModuleValues;
        if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
        {
            const FVersionedNiagaraEmitter OwningEmitter = EmitterHandle.GetInstance();
            const TArray<UNiagaraNodeFunctionCall*> FunctionCalls = CollectSortedNiagaraFunctionCalls(EmitterData->GraphSource);
            const int32 SafeModuleCount = FMath::Min(FunctionCalls.Num(), MaxModules);
            for (int32 ModuleIndex = 0; ModuleIndex < SafeModuleCount; ++ModuleIndex)
            {
                ModuleValues.Add(MakeShared<FJsonValueObject>(BuildModuleInputModuleJson(
                    FunctionCalls[ModuleIndex],
                    OwningEmitter,
                    EmitterData,
                    EmitterName,
                    EmitterIndex,
                    ModuleIndex,
                    bIncludeLinkedSources,
                    bIncludeResolvedStackInputs,
                    MaxCandidatesPerModule,
                    MaxResolvedInputsPerModule,
                    TotalCandidateCount,
                    TopCandidateObjects)));
            }
            TotalModuleCount += FunctionCalls.Num();
            EmitterObject->SetBoolField(TEXT("modules_truncated"), FunctionCalls.Num() > SafeModuleCount);
            EmitterObject->SetNumberField(TEXT("module_count"), FunctionCalls.Num());
        }
        else
        {
            EmitterObject->SetBoolField(TEXT("modules_truncated"), false);
            EmitterObject->SetNumberField(TEXT("module_count"), 0);
        }

        EmitterObject->SetArrayField(TEXT("modules"), ModuleValues);
        EmitterValues.Add(MakeShared<FJsonValueObject>(EmitterObject));
        ++EmitterIndex;
    }

    TopCandidateObjects.Sort(
        [](const TSharedPtr<FJsonObject>& Left, const TSharedPtr<FJsonObject>& Right)
        {
            const double LeftPriority = Left.IsValid() ? Left->GetNumberField(TEXT("priority")) : 0.0;
            const double RightPriority = Right.IsValid() ? Right->GetNumberField(TEXT("priority")) : 0.0;
            if (!FMath::IsNearlyEqual(LeftPriority, RightPriority))
            {
                return LeftPriority > RightPriority;
            }
            const FString LeftModule = Left.IsValid() ? Left->GetStringField(TEXT("module_name")) : FString();
            const FString RightModule = Right.IsValid() ? Right->GetStringField(TEXT("module_name")) : FString();
            return LeftModule < RightModule;
        });

    TArray<TSharedPtr<FJsonValue>> TopCandidateValues;
    for (int32 Index = 0; Index < TopCandidateObjects.Num() && Index < MaxTopCandidates; ++Index)
    {
        TopCandidateValues.Add(MakeShared<FJsonValueObject>(TopCandidateObjects[Index]));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetBoolField(TEXT("include_linked_sources"), bIncludeLinkedSources);
    Result->SetBoolField(TEXT("include_resolved_stack_inputs"), bIncludeResolvedStackInputs);
    Result->SetNumberField(TEXT("emitter_count"), System->GetEmitterHandles().Num());
    Result->SetNumberField(TEXT("module_count"), TotalModuleCount);
    Result->SetNumberField(TEXT("candidate_count"), TotalCandidateCount);
    Result->SetNumberField(TEXT("top_candidate_count"), TopCandidateValues.Num());
    Result->SetBoolField(TEXT("can_author_module_inputs"), false);
    Result->SetStringField(TEXT("authoring_status"), TEXT("read_only; use this result as generation planning input before enabling temp-asset module writes"));
    Result->SetArrayField(TEXT("emitters"), EmitterValues);
    Result->SetArrayField(TEXT("top_candidates"), TopCandidateValues);
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleSetNiagaraModuleInputValue(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString InputName;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("input_name"), InputName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'input_name' parameter"));
    }

    const TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
    if (!Value.IsValid())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'value' parameter"));
    }

    bool bAllowSourceEdit = false;
    Params->TryGetBoolField(TEXT("allow_source_edit"), bAllowSourceEdit);
    if (!bAllowSourceEdit && !IsTempGeneratedNiagaraPath(SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Refusing to edit Niagara module input outside %s: %s"), NiagaraTempGenerationRoot, *SystemPath));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    int32 TargetEmitterIndex = INDEX_NONE;
    int32 TargetModuleIndex = INDEX_NONE;
    FString TargetEmitterName;
    FString TargetModuleName;
    FString TargetModuleGuid;
    Params->TryGetNumberField(TEXT("emitter_index"), TargetEmitterIndex);
    Params->TryGetNumberField(TEXT("module_index"), TargetModuleIndex);
    Params->TryGetStringField(TEXT("emitter_name"), TargetEmitterName);
    Params->TryGetStringField(TEXT("module_name"), TargetModuleName);
    Params->TryGetStringField(TEXT("module_node_guid"), TargetModuleGuid);

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    UNiagaraNodeFunctionCall* MatchedFunctionCall = nullptr;
    FVersionedNiagaraEmitterData* MatchedEmitterData = nullptr;
    FVersionedNiagaraEmitter MatchedOwningEmitter;
    FString MatchedEmitterName;
    int32 MatchedEmitterIndex = INDEX_NONE;
    int32 MatchedModuleIndex = INDEX_NONE;

    int32 CurrentEmitterIndex = 0;
    for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
    {
        const FString EmitterName = EmitterHandle.GetName().ToString();
        const bool bEmitterMatches =
            (TargetEmitterIndex == INDEX_NONE || TargetEmitterIndex == CurrentEmitterIndex) &&
            (TargetEmitterName.IsEmpty() || TargetEmitterName.Equals(EmitterName, ESearchCase::IgnoreCase));

        if (bEmitterMatches)
        {
            if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
            {
                const TArray<UNiagaraNodeFunctionCall*> FunctionCalls = CollectSortedNiagaraFunctionCalls(EmitterData->GraphSource);
                for (int32 ModuleIndex = 0; ModuleIndex < FunctionCalls.Num(); ++ModuleIndex)
                {
                    UNiagaraNodeFunctionCall* FunctionCall = FunctionCalls[ModuleIndex];
                    if (!FunctionCall)
                    {
                        continue;
                    }

                    FString FunctionName = FunctionCall->GetFunctionName();
                    if (FunctionName.IsEmpty())
                    {
                        FunctionName = FunctionCall->Signature.GetNameString();
                    }
                    const FString FunctionGuid = FunctionCall->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
                    const bool bModuleMatches =
                        (TargetModuleIndex == INDEX_NONE || TargetModuleIndex == ModuleIndex) &&
                        (TargetModuleName.IsEmpty() || TargetModuleName.Equals(FunctionName, ESearchCase::IgnoreCase)) &&
                        (TargetModuleGuid.IsEmpty() || TargetModuleGuid.Equals(FunctionGuid, ESearchCase::IgnoreCase));

                    if (bModuleMatches)
                    {
                        MatchedFunctionCall = FunctionCall;
                        MatchedEmitterData = EmitterData;
                        MatchedOwningEmitter = EmitterHandle.GetInstance();
                        MatchedEmitterName = EmitterName;
                        MatchedEmitterIndex = CurrentEmitterIndex;
                        MatchedModuleIndex = ModuleIndex;
                        break;
                    }
                }
            }
        }

        if (MatchedFunctionCall)
        {
            break;
        }
        ++CurrentEmitterIndex;
    }

    if (!MatchedFunctionCall || !MatchedEmitterData)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No matching Niagara module was found"));
    }

    const ENiagaraScriptUsage OutputUsage = FNiagaraStackGraphUtilities::GetOutputNodeUsage(*MatchedFunctionCall);
    UNiagaraScript* OwningScript = FindEmitterScriptForUsage(MatchedEmitterData, OutputUsage);
    if (!OwningScript)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No owning Niagara script was found for the matched module"));
    }

    FCompileConstantResolver ConstantResolver(MatchedOwningEmitter, OutputUsage, MatchedFunctionCall->DebugState);
    TArray<FNiagaraVariable> InputVariables;
    TSet<FNiagaraVariable> HiddenVariables;
    FNiagaraStackGraphUtilities::GetStackFunctionInputs(
        *MatchedFunctionCall,
        InputVariables,
        HiddenVariables,
        ConstantResolver,
        FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly,
        true);

    FNiagaraVariable* MatchedInputVariable = InputVariables.FindByPredicate(
        [&InputName](const FNiagaraVariable& Candidate)
        {
            const FString CandidateName = Candidate.GetName().ToString();
            FString ShortName = CandidateName;
            ShortName.RemoveFromStart(TEXT("Module."));
            return CandidateName.Equals(InputName, ESearchCase::IgnoreCase) ||
                ShortName.Equals(InputName, ESearchCase::IgnoreCase);
        });

    if (!MatchedInputVariable)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("No matching module input was found: %s"), *InputName));
    }

    FNiagaraVariable RapidIterationParameter = BuildRapidIterationParameterForInput(
        MatchedFunctionCall,
        *MatchedInputVariable,
        MatchedEmitterName,
        OwningScript->GetUsage());

    const uint8* ExistingData = RapidIterationParameter.IsValid()
        ? OwningScript->RapidIterationParameters.GetParameterData(RapidIterationParameter)
        : nullptr;
    if (!ExistingData)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Refusing to create a new module input override. Existing RapidIteration value was not found for %s"), *MatchedInputVariable->GetName().ToString()));
    }

    RapidIterationParameter.SetData(ExistingData);
    TSharedPtr<FJsonValue> PreviousValue = NiagaraVariableDataToJsonValue(RapidIterationParameter);

    FString ErrorMessage;
    if (!SetNiagaraVariableDataFromJson(RapidIterationParameter, Value, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    System->Modify();
    OwningScript->Modify();
    OwningScript->RapidIterationParameters.SetParameterData(RapidIterationParameter.GetData(), RapidIterationParameter, false);
    System->MarkPackageDirty();
    System->RequestCompile(false);

    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveLoadedAsset(System, false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetStringField(TEXT("emitter_name"), MatchedEmitterName);
    Result->SetNumberField(TEXT("emitter_index"), MatchedEmitterIndex);
    Result->SetStringField(TEXT("module_name"), MatchedFunctionCall->GetFunctionName());
    Result->SetStringField(TEXT("module_node_guid"), MatchedFunctionCall->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
    Result->SetNumberField(TEXT("module_index"), MatchedModuleIndex);
    Result->SetStringField(TEXT("input_name"), MatchedInputVariable->GetName().ToString());
    Result->SetStringField(TEXT("input_type"), NiagaraTypeName(MatchedInputVariable->GetType()));
    Result->SetObjectField(TEXT("rapid_iteration_parameter"), NiagaraVariableWithDataToJsonObject(RapidIterationParameter));
    Result->SetField(TEXT("previous_value"), PreviousValue);
    Result->SetField(TEXT("new_value"), NiagaraVariableDataToJsonValue(RapidIterationParameter));
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetStringField(TEXT("write_scope"), TEXT("existing_rapid_iteration_parameter_only"));
    return Result;
}

TSharedPtr<FJsonObject> FUnrealMCPNiagaraCommands::HandleSetNiagaraUserParameter(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString ParameterName;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system_path"), SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'system_path' parameter"));
    }
    if (!Params->TryGetStringField(TEXT("parameter_name"), ParameterName))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'parameter_name' parameter"));
    }

    const TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
    if (!Value.IsValid())
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'value' parameter"));
    }

    bool bAllowSourceEdit = false;
    Params->TryGetBoolField(TEXT("allow_source_edit"), bAllowSourceEdit);
    if (!bAllowSourceEdit && !IsTempGeneratedNiagaraPath(SystemPath))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Refusing to edit Niagara system outside %s: %s"), NiagaraTempGenerationRoot, *SystemPath));
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *NormalizeNiagaraObjectPathForLoad(SystemPath));
    if (!System)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Failed to load Niagara system: %s"), *SystemPath));
    }

    TArray<FNiagaraVariable> UserParameters;
    System->GetExposedParameters().GetUserParameters(UserParameters);
    FNiagaraVariable* MatchedParameter = UserParameters.FindByPredicate(
        [&ParameterName](const FNiagaraVariable& Candidate)
        {
            const FString CandidateName = Candidate.GetName().ToString();
            return CandidateName.Equals(ParameterName, ESearchCase::IgnoreCase) ||
                CandidateName.Equals(FString::Printf(TEXT("User.%s"), *ParameterName), ESearchCase::IgnoreCase);
        });

    if (!MatchedParameter)
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("No matching Niagara user parameter was found: %s"), *ParameterName));
    }

    if (!IsSettableNiagaraUserParameterType(MatchedParameter->GetType()))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unsupported Niagara user parameter type: %s"), *NiagaraTypeName(MatchedParameter->GetType())));
    }

    FString ErrorMessage;
    FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
    if (!SetNiagaraUserParameterValue(Store, *MatchedParameter, Value, ErrorMessage))
    {
        return FUnrealMCPCommonUtils::CreateErrorResponse(ErrorMessage);
    }

    System->Modify();
    System->MarkPackageDirty();
    System->RequestCompile(false);

    bool bSaved = false;
    if (bSave)
    {
        bSaved = UEditorAssetLibrary::SaveLoadedAsset(System, false);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("system_path"), System->GetPathName());
    Result->SetStringField(TEXT("parameter_name"), MatchedParameter->GetName().ToString());
    Result->SetStringField(TEXT("type"), NiagaraTypeName(MatchedParameter->GetType()));
    Result->SetField(TEXT("value"), NiagaraParameterValueToJson(Store, *MatchedParameter));
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}
