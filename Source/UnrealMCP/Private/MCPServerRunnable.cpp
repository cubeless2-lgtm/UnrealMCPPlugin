#include "MCPServerRunnable.h"
#include "UnrealMCPBridge.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "JsonObjectConverter.h"
#include "Misc/ScopeLock.h"
#include "HAL/PlatformTime.h"

// Buffer size for receiving data
constexpr int32 MCPReceiveBufferSize = 8192;
constexpr int32 MCPMaxBufferedMessageSize = 8 * 1024 * 1024;

namespace
{
FString MakeMCPLogPreview(const FString& Text, int32 MaxChars = 512)
{
    if (Text.Len() <= MaxChars)
    {
        return Text;
    }

    return FString::Printf(TEXT("%s... [truncated, total chars=%d]"), *Text.Left(MaxChars), Text.Len());
}

bool IsCompleteJsonObjectMessage(const FString& Text)
{
    bool bStarted = false;
    bool bInString = false;
    bool bEscaped = false;
    bool bCompleted = false;
    int32 Depth = 0;

    for (int32 Index = 0; Index < Text.Len(); ++Index)
    {
        const TCHAR Ch = Text[Index];

        if (bCompleted)
        {
            if (!FChar::IsWhitespace(Ch))
            {
                return false;
            }
            continue;
        }

        if (!bStarted)
        {
            if (FChar::IsWhitespace(Ch))
            {
                continue;
            }
            if (Ch != TEXT('{'))
            {
                return false;
            }
            bStarted = true;
            Depth = 1;
            continue;
        }

        if (bInString)
        {
            if (bEscaped)
            {
                bEscaped = false;
            }
            else if (Ch == TEXT('\\'))
            {
                bEscaped = true;
            }
            else if (Ch == TEXT('"'))
            {
                bInString = false;
            }
            continue;
        }

        if (Ch == TEXT('"'))
        {
            bInString = true;
        }
        else if (Ch == TEXT('{') || Ch == TEXT('['))
        {
            ++Depth;
        }
        else if (Ch == TEXT('}') || Ch == TEXT(']'))
        {
            --Depth;
            if (Depth == 0)
            {
                bCompleted = true;
            }
            else if (Depth < 0)
            {
                return false;
            }
        }
    }

    return bStarted && bCompleted && !bInString && !bEscaped && Depth == 0;
}

bool SendAllToMCPSocket(TSharedPtr<FSocket> Socket, const FString& Response, int32& OutBytesSent)
{
    OutBytesSent = 0;
    if (!Socket.IsValid())
    {
        return false;
    }

    FTCHARToUTF8 ResponseUtf8(*Response);
    const uint8* Data = reinterpret_cast<const uint8*>(ResponseUtf8.Get());
    const int32 TotalBytes = ResponseUtf8.Length();

    while (OutBytesSent < TotalBytes)
    {
        int32 BytesSentThisCall = 0;
        if (!Socket->Send(Data + OutBytesSent, TotalBytes - OutBytesSent, BytesSentThisCall))
        {
            return false;
        }
        if (BytesSentThisCall <= 0)
        {
            FPlatformProcess::Sleep(0.001f);
            continue;
        }
        OutBytesSent += BytesSentThisCall;
    }

    return true;
}
}

FMCPServerRunnable::FMCPServerRunnable(UUnrealMCPBridge* InBridge, TSharedPtr<FSocket> InListenerSocket)
    : Bridge(InBridge)
    , ListenerSocket(InListenerSocket)
    , bRunning(true)
{
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Created server runnable"));
}

FMCPServerRunnable::~FMCPServerRunnable()
{
    // Note: We don't delete the sockets here as they're owned by the bridge
}

bool FMCPServerRunnable::Init()
{
    return true;
}

uint32 FMCPServerRunnable::Run()
{
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Server thread starting..."));
    
    while (bRunning)
    {
        // UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Waiting for client connection..."));
        
        bool bPending = false;
        if (ListenerSocket->HasPendingConnection(bPending) && bPending)
        {
            UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Client connection pending, accepting..."));
            
            ClientSocket = MakeShareable(ListenerSocket->Accept(TEXT("MCPClient")));
            if (ClientSocket.IsValid())
            {
                UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Client connection accepted"));
                
                // Set socket options to improve connection stability
                ClientSocket->SetNoDelay(true);
                int32 SocketBufferSize = 65536;  // 64KB buffer
                ClientSocket->SetSendBufferSize(SocketBufferSize, SocketBufferSize);
                ClientSocket->SetReceiveBufferSize(SocketBufferSize, SocketBufferSize);
                
                uint8 Buffer[MCPReceiveBufferSize + 1];
                FString MessageBuffer;
                while (bRunning)
                {
                    int32 BytesRead = 0;
                    if (ClientSocket->Recv(Buffer, MCPReceiveBufferSize, BytesRead))
                    {
                        if (BytesRead == 0)
                        {
                            if (!MessageBuffer.IsEmpty())
                            {
                                UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Client disconnected with incomplete buffered message, chars=%d preview=%s"), MessageBuffer.Len(), *MakeMCPLogPreview(MessageBuffer));
                            }
                            UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Client disconnected (zero bytes)"));
                            break;
                        }

                        // Convert received data to string
                        Buffer[BytesRead] = '\0';
                        FString ReceivedText = UTF8_TO_TCHAR(Buffer);
                        MessageBuffer.Append(ReceivedText);
                        UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Received chunk bytes=%d buffered_chars=%d preview=%s"), BytesRead, MessageBuffer.Len(), *MakeMCPLogPreview(MessageBuffer));

                        if (MessageBuffer.Len() > MCPMaxBufferedMessageSize)
                        {
                            UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Buffered message exceeded limit, chars=%d"), MessageBuffer.Len());
                            break;
                        }

                        if (!IsCompleteJsonObjectMessage(MessageBuffer))
                        {
                            continue;
                        }

                        // Parse JSON
                        TSharedPtr<FJsonObject> JsonObject;
                        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(MessageBuffer);
                        
                        if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
                        {
                            // Get command type
                            FString CommandType;
                            if (JsonObject->TryGetStringField(TEXT("type"), CommandType))
                            {
                                // Execute command
                                TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject());
                                const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
                                if (JsonObject->TryGetObjectField(TEXT("params"), ParamsObject) && ParamsObject && ParamsObject->IsValid())
                                {
                                    Params = *ParamsObject;
                                }
                                FString Response = Bridge->ExecuteCommand(CommandType, Params);
                                
                                // Log response for debugging
                                UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Sending response: %s"), *MakeMCPLogPreview(Response));
                                
                                // Send response
                                int32 BytesSent = 0;
                                if (!SendAllToMCPSocket(ClientSocket, Response, BytesSent))
                                {
                                    UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Failed to send response"));
                                }
                                else {
                                    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Response sent successfully, bytes: %d"), BytesSent);
                                }
                            }
                            else
                            {
                                UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Missing 'type' field in command"));
                            }
                        }
                        else
                        {
                            UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Failed to parse complete JSON from: %s"), *MakeMCPLogPreview(MessageBuffer));
                        }
                        MessageBuffer.Empty();
                    }
                    else
                    {
                        int32 LastError = (int32)ISocketSubsystem::Get()->GetLastErrorCode();
                        // Don't break the connection for WouldBlock error, which is normal for non-blocking sockets
                        bool bShouldBreak = true;
                        
                        // Check for "would block" error which isn't a real error for non-blocking sockets
                        if (LastError == SE_EWOULDBLOCK) 
                        {
                            UE_LOG(LogTemp, Verbose, TEXT("MCPServerRunnable: Socket would block, continuing..."));
                            bShouldBreak = false;
                            // Small sleep to prevent tight loop when no data
                            FPlatformProcess::Sleep(0.01f);
                        }
                        // Check for other transient errors we might want to tolerate
                        else if (LastError == SE_EINTR) // Interrupted system call
                        {
                            UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Socket read interrupted, continuing..."));
                            bShouldBreak = false;
                        }
                        else 
                        {
                            UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Client disconnected or error. Last error code: %d"), LastError);
                        }
                        
                        if (bShouldBreak)
                        {
                            break;
                        }
                    }
                }
            }
            else
            {
                UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Failed to accept client connection"));
            }
        }
        
        // Small sleep to prevent tight loop
        FPlatformProcess::Sleep(0.1f);
    }
    
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Server thread stopping"));
    return 0;
}

void FMCPServerRunnable::Stop()
{
    bRunning = false;
}

void FMCPServerRunnable::Exit()
{
}

void FMCPServerRunnable::HandleClientConnection(TSharedPtr<FSocket> InClientSocket)
{
    if (!InClientSocket.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("MCPServerRunnable: Invalid client socket passed to HandleClientConnection"));
        return;
    }

    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Starting to handle client connection"));
    
    // Set socket options for better connection stability
    InClientSocket->SetNonBlocking(false);
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Set socket to blocking mode"));
    
    // Properly read full message with timeout
    const int32 MaxBufferSize = 4096;
    uint8 Buffer[MaxBufferSize];
    FString MessageBuffer;
    
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Starting message receive loop"));
    
    while (bRunning && InClientSocket.IsValid())
    {
        // Log socket state
        bool bIsConnected = InClientSocket->GetConnectionState() == SCS_Connected;
        UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Socket state - Connected: %s"), 
               bIsConnected ? TEXT("true") : TEXT("false"));
        
        // Log pending data status before receive
        uint32 PendingDataSize = 0;
        bool HasPendingData = InClientSocket->HasPendingData(PendingDataSize);
        UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Before Recv - HasPendingData=%s, Size=%d"), 
               HasPendingData ? TEXT("true") : TEXT("false"), PendingDataSize);
        
        // Try to receive data with timeout
        int32 BytesRead = 0;
        bool bReadSuccess = false;
        
        UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Attempting to receive data..."));
        bReadSuccess = InClientSocket->Recv(Buffer, MaxBufferSize, BytesRead, ESocketReceiveFlags::None);
        
        UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Recv attempt complete - Success=%s, BytesRead=%d"), 
               bReadSuccess ? TEXT("true") : TEXT("false"), BytesRead);
        
        if (BytesRead > 0)
        {
            // Log raw data for debugging
            FString HexData;
            for (int32 i = 0; i < FMath::Min(BytesRead, 50); ++i)
            {
                HexData += FString::Printf(TEXT("%02X "), Buffer[i]);
            }
            UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Raw data (first 50 bytes hex): %s%s"), 
                   *HexData, BytesRead > 50 ? TEXT("...") : TEXT(""));
            
            // Convert and log received data
            Buffer[BytesRead] = 0; // Null terminate
            FString ReceivedData = UTF8_TO_TCHAR(Buffer);
            UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Received data as string: '%s'"), *ReceivedData);
            
            // Append to message buffer
            MessageBuffer.Append(ReceivedData);
            
            // Process complete messages (messages are terminated with newline)
            if (MessageBuffer.Contains(TEXT("\n")))
            {
                UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Newline detected in buffer, processing messages"));
                
                TArray<FString> Messages;
                MessageBuffer.ParseIntoArray(Messages, TEXT("\n"), true);
                
                UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Found %d message(s) in buffer"), Messages.Num());
                
                // Process all complete messages
                for (int32 i = 0; i < Messages.Num() - 1; ++i)
                {
                    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Processing message %d: '%s'"), 
                           i + 1, *Messages[i]);
                    ProcessMessage(InClientSocket, Messages[i]);
                }
                
                // Keep any incomplete message in the buffer
                MessageBuffer = Messages.Last();
                UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Remaining buffer after processing: %s"), 
                       *MessageBuffer);
            }
            else
            {
                UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: No complete message yet (no newline detected)"));
            }
        }
        else if (!bReadSuccess)
        {
            UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Connection closed or error occurred - Last error: %d"), 
                   (int32)ISocketSubsystem::Get()->GetLastErrorCode());
            break;
        }
        
        // Small sleep to prevent tight loop
        FPlatformProcess::Sleep(0.01f);
    }
    
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Exited message receive loop"));
}

void FMCPServerRunnable::ProcessMessage(TSharedPtr<FSocket> Client, const FString& Message)
{
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Processing message: %s"), *Message);
    
    // Parse message as JSON
    TSharedPtr<FJsonObject> JsonMessage;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
    
    if (!FJsonSerializer::Deserialize(Reader, JsonMessage) || !JsonMessage.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Failed to parse message as JSON"));
        return;
    }
    
    // Extract command type and parameters using MCP protocol format
    FString CommandType;
    TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject());
    
    if (!JsonMessage->TryGetStringField(TEXT("command"), CommandType))
    {
        UE_LOG(LogTemp, Warning, TEXT("MCPServerRunnable: Message missing 'command' field"));
        return;
    }
    
    // Parameters are optional in MCP protocol
    if (JsonMessage->HasField(TEXT("params")))
    {
        TSharedPtr<FJsonValue> ParamsValue = JsonMessage->TryGetField(TEXT("params"));
        if (ParamsValue.IsValid() && ParamsValue->Type == EJson::Object)
        {
            Params = ParamsValue->AsObject();
        }
    }
    
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Executing command: %s"), *CommandType);
    
    // Execute command
    FString Response = Bridge->ExecuteCommand(CommandType, Params);
    
    // Send response with newline terminator
    Response += TEXT("\n");
    int32 BytesSent = 0;
    
    UE_LOG(LogTemp, Display, TEXT("MCPServerRunnable: Sending response: %s"), *Response);
    
    if (!Client->Send((uint8*)TCHAR_TO_UTF8(*Response), Response.Len(), BytesSent))
    {
        UE_LOG(LogTemp, Error, TEXT("MCPServerRunnable: Failed to send response"));
    }
} 
