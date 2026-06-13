#include "UnrealMCPBridge.h"
#include "MCPServerRunnable.h"
#include "UI/IetaMCPStatusWindow.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Camera/CameraActor.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "JsonObjectConverter.h"
#include "GameFramework/Actor.h"
#include "Engine/Selection.h"
#include "Kismet/GameplayStatics.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
// Add Blueprint related includes
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Factories/BlueprintFactory.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
// UE5.5 correct includes
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "UObject/Field.h"
#include "UObject/FieldPath.h"
// Blueprint Graph specific includes
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_CallFunction.h"
#include "K2Node_InputAction.h"
#include "K2Node_Self.h"
#include "GameFramework/InputSettings.h"
#include "EditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
// Include our new command handler classes
#include "Commands/UnrealMCPEditorCommands.h"
#include "Commands/UnrealMCPBlueprintCommands.h"
#include "Commands/UnrealMCPBlueprintNodeCommands.h"
#include "Commands/UnrealMCPMaterialCommands.h"
#include "Commands/UnrealMCPNiagaraCommands.h"
#include "Commands/UnrealMCPPCGCommands.h"
#include "Commands/UnrealMCPProjectCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "Commands/UnrealMCPUMGCommands.h"

// Default settings
#define MCP_SERVER_HOST "127.0.0.1"
#define MCP_SERVER_PORT 55557

UUnrealMCPBridge::UUnrealMCPBridge()
{
    EditorCommands = MakeShared<FUnrealMCPEditorCommands>();
    BlueprintCommands = MakeShared<FUnrealMCPBlueprintCommands>();
    BlueprintNodeCommands = MakeShared<FUnrealMCPBlueprintNodeCommands>();
    MaterialCommands = MakeShared<FUnrealMCPMaterialCommands>();
    NiagaraCommands = MakeShared<FUnrealMCPNiagaraCommands>();
    PCGCommands = MakeShared<FUnrealMCPPCGCommands>();
    ProjectCommands = MakeShared<FUnrealMCPProjectCommands>();
    UMGCommands = MakeShared<FUnrealMCPUMGCommands>();
}

UUnrealMCPBridge::~UUnrealMCPBridge()
{
    EditorCommands.Reset();
    BlueprintCommands.Reset();
    BlueprintNodeCommands.Reset();
    MaterialCommands.Reset();
    NiagaraCommands.Reset();
    PCGCommands.Reset();
    ProjectCommands.Reset();
    UMGCommands.Reset();
}

// Initialize subsystem
void UUnrealMCPBridge::Initialize(FSubsystemCollectionBase& Collection)
{
    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Initializing"));
    
    bIsRunning = false;
    ListenerSocket = nullptr;
    ConnectionSocket = nullptr;
    ServerRunnable = nullptr;
    ServerThread = nullptr;
    Port = MCP_SERVER_PORT;
    FIPv4Address::Parse(MCP_SERVER_HOST, ServerAddress);

    // Start the server automatically
    StartServer();
    ScheduleStartupIetaStatusSequence();
}

// Clean up resources when subsystem is destroyed
void UUnrealMCPBridge::Deinitialize()
{
    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Shutting down"));
    StopServer();
}

// Start the MCP server
void UUnrealMCPBridge::StartServer()
{
    if (bIsRunning)
    {
        UE_LOG(LogTemp, Warning, TEXT("UnrealMCPBridge: Server is already running"));
        return;
    }

    // Create socket subsystem
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!SocketSubsystem)
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to get socket subsystem"));
        return;
    }

    // Create listener socket
    TSharedPtr<FSocket> NewListenerSocket = MakeShareable(SocketSubsystem->CreateSocket(NAME_Stream, TEXT("UnrealMCPListener"), false));
    if (!NewListenerSocket.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to create listener socket"));
        return;
    }

    // Allow address reuse for quick restarts
    NewListenerSocket->SetReuseAddr(true);
    NewListenerSocket->SetNonBlocking(true);

    // Bind to address
    FIPv4Endpoint Endpoint(ServerAddress, Port);
    if (!NewListenerSocket->Bind(*Endpoint.ToInternetAddr()))
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to bind listener socket to %s:%d"), *ServerAddress.ToString(), Port);
        return;
    }

    // Start listening
    if (!NewListenerSocket->Listen(5))
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to start listening"));
        return;
    }

    ListenerSocket = NewListenerSocket;
    bIsRunning = true;
    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Server started on %s:%d"), *ServerAddress.ToString(), Port);

    // Start server thread
    ServerRunnable = new FMCPServerRunnable(this, ListenerSocket);
    ServerThread = FRunnableThread::Create(
        ServerRunnable,
        TEXT("UnrealMCPServerThread"),
        0, TPri_Normal
    );

    if (!ServerThread)
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Failed to create server thread"));
        delete ServerRunnable;
        ServerRunnable = nullptr;
        StopServer();
        return;
    }
}

// Stop the MCP server
void UUnrealMCPBridge::StopServer()
{
    if (!bIsRunning)
    {
        return;
    }

    bIsRunning = false;

    // Clean up thread
    if (ServerThread)
    {
        if (ServerRunnable)
        {
            ServerRunnable->Stop();
        }
        ServerThread->WaitForCompletion();
        delete ServerThread;
        ServerThread = nullptr;
    }

    delete ServerRunnable;
    ServerRunnable = nullptr;

    // Close sockets
    if (ConnectionSocket.IsValid())
    {
        ConnectionSocket->Close();
        ConnectionSocket.Reset();
    }

    if (ListenerSocket.IsValid())
    {
        ListenerSocket->Close();
        ListenerSocket.Reset();
    }

    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Server stopped"));
}

void UUnrealMCPBridge::ScheduleStartupIetaStatusSequence()
{
    TWeakObjectPtr<UUnrealMCPBridge> WeakThis(this);
    AsyncTask(ENamedThreads::GameThread, [WeakThis]()
    {
        if (!WeakThis.IsValid())
        {
            return;
        }

        constexpr double StartupCheckSeconds = 2.0;
        const double StartTime = FPlatformTime::Seconds();
        const FString ConnectingText = TEXT("흥, 연결 확인 중이야. 잠깐만 기다려.");
        FIetaMCPStatusWindow::UpdateStatus(ConnectingText, TEXT("ieta_status"));

        FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakThis, StartTime](float)
        {
            if (!WeakThis.IsValid())
            {
                return false;
            }

            const double ElapsedSeconds = FPlatformTime::Seconds() - StartTime;
            if (ElapsedSeconds < StartupCheckSeconds)
            {
                return true;
            }

            const bool bConnected = WeakThis->IsRunning();
            const FString MCPStatusText = bConnected
                ? FString::Printf(
                    TEXT("현재 접속된 MCP: unrealMCP / UnrealMCP Editor bridge (%s:%d)"),
                    *WeakThis->ServerAddress.ToString(),
                    WeakThis->Port)
                : TEXT("현재 접속된 MCP: 없음");
            const FString EditorLogStatusText = FIetaMCPStatusWindow::BuildEditorLogStatusText();
            const FString LogStatusText = EditorLogStatusText.IsEmpty()
                ? MCPStatusText
                : FString::Printf(
                    TEXT("%s\n%s"),
                    *MCPStatusText,
                    *EditorLogStatusText);
            const FString StatusText = bConnected
                ? FString::Printf(TEXT("연결 완료야. Unreal MCP 준비됐어.\n%s"), *LogStatusText)
                : FString::Printf(TEXT("연결 실패야. 서버가 안 떠 있어. 이 창은 닫지 않을게.\n%s"), *LogStatusText);

            FIetaMCPStatusWindow::UpdateStatus(
                StatusText,
                TEXT("ieta_status"));
            if (bConnected)
            {
                FIetaMCPStatusWindow::Hide();
            }
            return false;
        }), 0.1f);
    });
}

// Execute a command received from a client
FString UUnrealMCPBridge::ExecuteCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    UE_LOG(LogTemp, Display, TEXT("UnrealMCPBridge: Executing command: %s"), *CommandType);
    const bool bShowIetaSlate = CommandType == TEXT("ieta_status");
    
    // Create a promise to wait for the result
    TSharedRef<TPromise<FString>, ESPMode::ThreadSafe> Promise = MakeShared<TPromise<FString>, ESPMode::ThreadSafe>();
    TFuture<FString> Future = Promise->GetFuture();
    
    auto RunCommand = [this, CommandType, Params, Promise, bShowIetaSlate]()
    {
        TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject);
        
        try
        {
            TSharedPtr<FJsonObject> ResultJson;
            
            if (CommandType == TEXT("ping"))
            {
                ResultJson = MakeShareable(new FJsonObject);
                ResultJson->SetStringField(TEXT("message"), TEXT("pong"));
            }
            else if (CommandType == TEXT("ieta_status"))
            {
                FString StatusText;
                FString StatusCommand;
                if (Params.IsValid())
                {
                    Params->TryGetStringField(TEXT("text"), StatusText);
                    Params->TryGetStringField(TEXT("command"), StatusCommand);
                }

                if (StatusText.IsEmpty())
                {
                    StatusText = TEXT("이에타가 작업 상태를 확인하는 중이야.");
                }

                if (StatusCommand.IsEmpty())
                {
                    StatusCommand = TEXT("ieta_status");
                }

                FIetaMCPStatusWindow::UpdateStatus(
                    StatusText,
                    StatusCommand);

                ResultJson = MakeShareable(new FJsonObject);
                ResultJson->SetBoolField(TEXT("success"), true);
                ResultJson->SetStringField(TEXT("text"), StatusText);
                ResultJson->SetStringField(TEXT("command"), StatusCommand);
            }
            // Editor Commands (including actor manipulation)
            else if (CommandType == TEXT("get_actors_in_level") || 
                     CommandType == TEXT("find_actors_by_name") ||
                     CommandType == TEXT("spawn_actor") ||
                     CommandType == TEXT("create_actor") ||
                     CommandType == TEXT("delete_actor") || 
                     CommandType == TEXT("set_actor_transform") ||
                     CommandType == TEXT("get_actor_properties") ||
                     CommandType == TEXT("set_actor_property") ||
                     CommandType == TEXT("spawn_blueprint_actor") ||
                     CommandType == TEXT("focus_viewport") || 
                     CommandType == TEXT("take_screenshot") ||
                     CommandType == TEXT("list_viewport_bookmarks") ||
                     CommandType == TEXT("capture_viewport_bookmark_screenshot") ||
                     CommandType == TEXT("open_editor_level") ||
                     CommandType == TEXT("open_niagara_preview_player") ||
                     CommandType == TEXT("get_niagara_preview_player_state") ||
                     CommandType == TEXT("get_niagara_preview_lab_state") ||
                     CommandType == TEXT("cleanup_niagara_preview_lab") ||
                     CommandType == TEXT("capture_niagara_preview_lab_view") ||
                     CommandType == TEXT("preview_niagara_system_in_preview_lab") ||
                     CommandType == TEXT("sample_niagara_system_in_preview_lab"))
            {
                ResultJson = EditorCommands->HandleCommand(CommandType, Params);
            }
            // Blueprint Commands
            else if (CommandType == TEXT("create_blueprint") || 
                     CommandType == TEXT("add_component_to_blueprint") ||
                     CommandType == TEXT("list_blueprint_components") ||
                     CommandType == TEXT("set_component_property") || 
                     CommandType == TEXT("get_component_property") ||
                     CommandType == TEXT("set_physics_properties") ||
                     CommandType == TEXT("compile_blueprint") || 
                     CommandType == TEXT("compile_and_save_blueprint") ||
                     CommandType == TEXT("compile_and_validate_blueprint") ||
                     CommandType == TEXT("set_blueprint_property") || 
                     CommandType == TEXT("set_static_mesh_properties") ||
                     CommandType == TEXT("set_pawn_properties"))
            {
                ResultJson = BlueprintCommands->HandleCommand(CommandType, Params);
            }
            // Blueprint Node Commands
            else if (CommandType == TEXT("connect_blueprint_nodes") || 
                     CommandType == TEXT("resolve_blueprint") ||
                     CommandType == TEXT("list_blueprint_graphs") ||
                     CommandType == TEXT("resolve_blueprint_graph") ||
                     CommandType == TEXT("list_blueprint_nodes") ||
                     CommandType == TEXT("add_blueprint_get_self_component_reference") ||
                     CommandType == TEXT("add_blueprint_self_reference") ||
                     CommandType == TEXT("find_blueprint_nodes") ||
                     CommandType == TEXT("add_blueprint_event_node") ||
                     CommandType == TEXT("add_blueprint_branch_node") ||
                     CommandType == TEXT("add_blueprint_sequence_node") ||
                     CommandType == TEXT("add_blueprint_return_node") ||
                     CommandType == TEXT("add_blueprint_dynamic_cast_node") ||
                     CommandType == TEXT("add_blueprint_loop_node") ||
                     CommandType == TEXT("add_blueprint_array_function_node") ||
                     CommandType == TEXT("add_blueprint_make_array_node") ||
                     CommandType == TEXT("add_blueprint_input_action_node") ||
                     CommandType == TEXT("add_blueprint_input_axis_event_node") ||
                     CommandType == TEXT("add_blueprint_enhanced_input_action_node") ||
                     CommandType == TEXT("add_blueprint_function_node") ||
                     CommandType == TEXT("add_blueprint_call_function_node") ||
                     CommandType == TEXT("add_blueprint_variable_get_node") ||
                     CommandType == TEXT("add_blueprint_variable_set_node") ||
                     CommandType == TEXT("add_blueprint_event_dispatcher") ||
                     CommandType == TEXT("add_blueprint_event_dispatcher_call_node") ||
                     CommandType == TEXT("add_blueprint_custom_event_node") ||
                     CommandType == TEXT("add_blueprint_event_dispatcher_bind_node") ||
                     CommandType == TEXT("add_blueprint_event_dispatcher_unbind_node") ||
                     CommandType == TEXT("add_blueprint_event_dispatcher_clear_node") ||
                     CommandType == TEXT("add_blueprint_event_dispatcher_assign_node") ||
                     CommandType == TEXT("add_blueprint_math_node") ||
                     CommandType == TEXT("add_blueprint_compare_node") ||
                     CommandType == TEXT("add_blueprint_boolean_node") ||
                     CommandType == TEXT("add_blueprint_select_node") ||
                     CommandType == TEXT("add_blueprint_literal_node") ||
                     CommandType == TEXT("add_blueprint_enum_literal_node") ||
                     CommandType == TEXT("add_blueprint_is_valid_node") ||
                     CommandType == TEXT("add_blueprint_make_struct_node") ||
                     CommandType == TEXT("add_blueprint_break_struct_node") ||
                     CommandType == TEXT("add_blueprint_switch_int_node") ||
                     CommandType == TEXT("add_blueprint_switch_enum_node") ||
                     CommandType == TEXT("set_blueprint_pin_default") ||
                     CommandType == TEXT("add_blueprint_function_parameter") ||
                     CommandType == TEXT("add_blueprint_local_variable") ||
                     CommandType == TEXT("add_blueprint_variable"))
            {
                ResultJson = BlueprintNodeCommands->HandleCommand(CommandType, Params);
            }
            // Material Graph Commands
            else if (CommandType == TEXT("resolve_material_graph") ||
                     CommandType == TEXT("list_material_nodes") ||
                     CommandType == TEXT("analyze_material_graph") ||
                     CommandType == TEXT("add_material_node") ||
                     CommandType == TEXT("add_custom_material_node") ||
                     CommandType == TEXT("set_material_node_property") ||
                     CommandType == TEXT("connect_material_nodes") ||
                     CommandType == TEXT("connect_material_property") ||
                     CommandType == TEXT("delete_material_node") ||
                     CommandType == TEXT("layout_material_nodes") ||
                     CommandType == TEXT("compile_and_save_material") ||
                     CommandType == TEXT("expand_material_function_calls") ||
                     CommandType == TEXT("get_material_parameter_collection_values"))
            {
                ResultJson = MaterialCommands->HandleCommand(CommandType, Params);
            }
            // Niagara Commands
            else if (CommandType == TEXT("analyze_niagara_system") ||
                     CommandType == TEXT("inspect_niagara_renderers") ||
                     CommandType == TEXT("set_niagara_renderer_material") ||
                     CommandType == TEXT("inspect_niagara_user_parameters") ||
                     CommandType == TEXT("set_niagara_user_parameter") ||
                     CommandType == TEXT("inspect_niagara_stack") ||
                     CommandType == TEXT("inspect_niagara_graph") ||
                     CommandType == TEXT("inspect_niagara_compile_status") ||
                     CommandType == TEXT("inspect_niagara_scratch_pad_interface") ||
                     CommandType == TEXT("duplicate_or_attach_emitter_from_source") ||
                     CommandType == TEXT("create_or_duplicate_scratch_pad_module") ||
                     CommandType == TEXT("add_scratch_pad_module_to_stack") ||
                     CommandType == TEXT("inspect_niagara_module_inputs") ||
                     CommandType == TEXT("create_niagara_module_input_override") ||
                     CommandType == TEXT("set_niagara_module_inputs_batch") ||
                     CommandType == TEXT("set_niagara_module_input_value"))
            {
                ResultJson = NiagaraCommands->HandleCommand(CommandType, Params);
            }
            // PCG Graph Commands
            else if (CommandType == TEXT("resolve_pcg_graph") ||
                     CommandType == TEXT("list_pcg_graph_nodes") ||
                     CommandType == TEXT("add_pcg_node") ||
                     CommandType == TEXT("connect_pcg_nodes") ||
                     CommandType == TEXT("set_pcg_node_setting") ||
                     CommandType == TEXT("compile_or_notify_pcg_graph") ||
                     CommandType == TEXT("save_pcg_graph"))
            {
                ResultJson = PCGCommands->HandleCommand(CommandType, Params);
            }
            // Project Commands
            else if (CommandType == TEXT("create_input_mapping") ||
                     CommandType == TEXT("execute_python") ||
                     CommandType == TEXT("recreate_content_folder_mcp") ||
                     CommandType == TEXT("postprocess_content_folder_mcp") ||
                     CommandType == TEXT("repair_world_actor_instances_mcp") ||
                     CommandType == TEXT("run_content_validation_pipeline_mcp") ||
                     CommandType == TEXT("analyze_blueprint_widget_fallbacks_mcp"))
            {
                ResultJson = ProjectCommands->HandleCommand(CommandType, Params);
            }
            // UMG Commands
            else if (CommandType == TEXT("create_umg_widget_blueprint") ||
                     CommandType == TEXT("add_text_block_to_widget") ||
                     CommandType == TEXT("add_button_to_widget") ||
                     CommandType == TEXT("bind_widget_event") ||
                     CommandType == TEXT("set_text_block_binding") ||
                     CommandType == TEXT("add_widget_to_viewport"))
            {
                ResultJson = UMGCommands->HandleCommand(CommandType, Params);
            }
            else
            {
                ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                ResponseJson->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown command: %s"), *CommandType));
                
                FString ResultString;
                TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
                FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
                Promise->SetValue(ResultString);
                return;
            }
            
            // Check if the result contains an error
            bool bSuccess = true;
            FString ErrorMessage;
            
            if (ResultJson->HasField(TEXT("success")))
            {
                bSuccess = ResultJson->GetBoolField(TEXT("success"));
                if (!bSuccess && ResultJson->HasField(TEXT("error")))
                {
                    ErrorMessage = ResultJson->GetStringField(TEXT("error"));
                }
                if (!bSuccess && ErrorMessage.IsEmpty() && ResultJson->HasField(TEXT("command_result")))
                {
                    ErrorMessage = ResultJson->GetStringField(TEXT("command_result"));
                }
                if (!bSuccess && ErrorMessage.IsEmpty() && ResultJson->HasTypedField<EJson::Array>(TEXT("logs")))
                {
                    const TArray<TSharedPtr<FJsonValue>>* Logs = nullptr;
                    if (ResultJson->TryGetArrayField(TEXT("logs"), Logs) && Logs)
                    {
                        for (const TSharedPtr<FJsonValue>& LogValue : *Logs)
                        {
                            const TSharedPtr<FJsonObject> LogObject = LogValue.IsValid() ? LogValue->AsObject() : nullptr;
                            if (LogObject.IsValid() && LogObject->HasField(TEXT("output")))
                            {
                                ErrorMessage = LogObject->GetStringField(TEXT("output"));
                                if (!ErrorMessage.IsEmpty())
                                {
                                    break;
                                }
                            }
                        }
                    }
                }
                if (!bSuccess && ErrorMessage.IsEmpty())
                {
                    ErrorMessage = FString::Printf(TEXT("Command '%s' reported failure without details"), *CommandType);
                }
            }
            
            if (bSuccess)
            {
                // Set success status and include the result
                ResponseJson->SetStringField(TEXT("status"), TEXT("success"));
                ResponseJson->SetObjectField(TEXT("result"), ResultJson);
            }
            else
            {
                // Set error status and include the error message
                ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
                ResponseJson->SetStringField(TEXT("error"), ErrorMessage);
                ResponseJson->SetObjectField(TEXT("result"), ResultJson);
            }
        }
        catch (const std::exception& e)
        {
            const FString ExceptionMessage = UTF8_TO_TCHAR(e.what());
            const FString SafeExceptionMessage = ExceptionMessage.IsEmpty()
                ? FString::Printf(TEXT("Command '%s' threw std::exception with empty message"), *CommandType)
                : ExceptionMessage;
            UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Command %s threw std::exception: %s"), *CommandType, *SafeExceptionMessage);
            ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
            ResponseJson->SetStringField(TEXT("error"), SafeExceptionMessage);
        }
        catch (...)
        {
            const FString ErrorMessage = FString::Printf(TEXT("Command '%s' threw an unknown exception"), *CommandType);
            UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: %s"), *ErrorMessage);
            ResponseJson->SetStringField(TEXT("status"), TEXT("error"));
            ResponseJson->SetStringField(TEXT("error"), ErrorMessage);
        }
        
        if (bShowIetaSlate)
        {
            FIetaMCPStatusWindow::Hide();
        }

        FString ResultString;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
        FJsonSerializer::Serialize(ResponseJson.ToSharedRef(), Writer);
        Promise->SetValue(ResultString);
    };

    bool bUseTickerDispatch = false;
    if (CommandType == TEXT("execute_python") && Params.IsValid() && Params->HasField(TEXT("defer_to_ticker")))
    {
        bUseTickerDispatch = Params->GetBoolField(TEXT("defer_to_ticker"));
    }

    // Queue execution on Game Thread. The assistant window is reserved for
    // explicit ieta_status checks so background work stays out of the way.
    // Most MCP commands run immediately once they reach the game thread. A
    // small set of Python workflows can opt into ticker dispatch when they need
    // to avoid running inside the TaskGraph callback itself.
    AsyncTask(ENamedThreads::GameThread, [CommandType, Params, RunCommand, bShowIetaSlate, bUseTickerDispatch]()
    {
        if (bShowIetaSlate)
        {
            FIetaMCPStatusWindow::ShowWithParams(CommandType, Params);
        }

        if (!bUseTickerDispatch)
        {
            RunCommand();
            return;
        }

        FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([RunCommand](float)
        {
            RunCommand();
            return false;
        }));
    });
    
    constexpr double CommandTimeoutSeconds = 120.0;
    if (!Future.WaitFor(FTimespan::FromSeconds(CommandTimeoutSeconds)))
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealMCPBridge: Command %s timed out after %.0f seconds"), *CommandType, CommandTimeoutSeconds);

        TSharedPtr<FJsonObject> TimeoutResponse = MakeShared<FJsonObject>();
        TimeoutResponse->SetStringField(TEXT("status"), TEXT("error"));
        TimeoutResponse->SetStringField(TEXT("error"), FString::Printf(
            TEXT("Command '%s' timed out after %.0f seconds"),
            *CommandType,
            CommandTimeoutSeconds));

        FString TimeoutResultString;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&TimeoutResultString);
        FJsonSerializer::Serialize(TimeoutResponse.ToSharedRef(), Writer);
        return TimeoutResultString;
    }

    return Future.Get();
}
