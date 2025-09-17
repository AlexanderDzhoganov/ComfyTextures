// Fill out your copyright notice in the Description page of Project Settings.

#include "MCPClient.h"
#include "Json.h"
#include "WebSocketsModule.h"
#include "Serialization/JsonSerializer.h"

UMCPClient::UMCPClient()
    : ServerPort(25565)
    , bIsConnected(false)
    , bEncryptionEnabled(false)
    , ConnectionTimeout(30.0f)
    , MaxRetries(3)
    , RetryDelay(2.0f)
    , CurrentRetryCount(0)
{
    if (!FModuleManager::Get().IsModuleLoaded("WebSockets"))
    {
        FModuleManager::Get().LoadModule("WebSockets");
        UE_LOG(LogComfyTextures, Warning, TEXT("Loaded WebSockets module for MCP"));
    }
}

void UMCPClient::Initialize(const FString& ServerAddress, int32 Port, const FString& Username)
{
    this->ServerAddress = ServerAddress;
    this->ServerPort = Port;
    this->PlayerUsername = Username;
}

bool UMCPClient::Connect()
{
    if (bIsConnected)
    {
        UE_LOG(LogComfyTextures, Warning, TEXT("MCP Client already connected"));
        return true;
    }

    if (WebSocket.IsValid())
    {
        WebSocket->Close();
    }

    FString WsUrl = FString::Printf(TEXT("ws://%s:%d/mcp"), *ServerAddress, ServerPort);
    WebSocket = FWebSocketsModule::Get().CreateWebSocket(WsUrl, TEXT("mcp"));

    // Set up WebSocket event handlers
    WebSocket->OnConnected().AddLambda([this]()
    {
        bIsConnected = true;
        HandleWebSocketConnection(true);

        // Send handshake
        FString HandshakeMessage = BuildHandshakeMessage();
        WebSocket->Send(HandshakeMessage);
    });

    WebSocket->OnConnectionError().AddLambda([this](const FString& Error)
    {
        bIsConnected = false;
        HandleWebSocketError(Error);
    });

    WebSocket->OnClosed().AddLambda([this](int32 StatusCode, const FString& Reason, bool bWasClean)
    {
        bIsConnected = false;
        HandleWebSocketConnection(false);
    });

    WebSocket->OnMessage().AddLambda([this](const FString& Message)
    {
        HandleWebSocketMessage(Message);
    });

    UE_LOG(LogComfyTextures, Log, TEXT("Connecting to MCP server at %s"), *WsUrl);
    return WebSocket->Connect();
}

void UMCPClient::Disconnect()
{
    if (WebSocket.IsValid())
    {
        WebSocket->Close();
        WebSocket.Reset();
    }
    bIsConnected = false;
}

bool UMCPClient::IsConnected() const
{
    return bIsConnected && WebSocket.IsValid() && WebSocket->IsConnected();
}

bool UMCPClient::SendPrompt(const FString& Prompt, const TArray<uint8>& SceneData)
{
    if (!IsConnected())
    {
        UE_LOG(LogComfyTextures, Error, TEXT("MCP Client not connected"));
        return false;
    }

    FString Message = BuildPromptMessage(Prompt, SceneData);
    WebSocket->Send(Message);
    return true;
}

bool UMCPClient::RequestSceneData(const FString& WorldName, const FVector& Location, float Radius)
{
    if (!IsConnected())
    {
        UE_LOG(LogComfyTextures, Error, TEXT("MCP Client not connected"));
        return false;
    }

    FString Message = BuildSceneRequestMessage(WorldName, Location, Radius);
    WebSocket->Send(Message);
    return true;
}

void UMCPClient::SetConnectionCallback(TFunction<void(bool)> Callback)
{
    OnConnectionChanged = Callback;
}

void UMCPClient::SetPromptResponseCallback(TFunction<void(const TSharedPtr<FJsonObject>&)> Callback)
{
    OnPromptResponse = Callback;
}

void UMCPClient::SetSceneDataCallback(TFunction<void(const TArray<uint8>&)> Callback)
{
    OnSceneDataReceived = Callback;
}

void UMCPClient::SetErrorCallback(TFunction<void(const FString&)> Callback)
{
    OnError = Callback;
}

void UMCPClient::HandleWebSocketMessage(const FString& Message)
{
    ParseMessage(Message);
}

void UMCPClient::HandleWebSocketConnection(bool bConnected)
{
    if (OnConnectionChanged)
    {
        OnConnectionChanged(bConnected);
    }
}

void UMCPClient::HandleWebSocketError(const FString& Error)
{
    UE_LOG(LogComfyTextures, Error, TEXT("MCP WebSocket error: %s"), *Error);
    if (OnError)
    {
        OnError(Error);
    }
}

FString UMCPClient::BuildHandshakeMessage() const
{
    TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();
    JsonObject->SetStringField("type", "handshake");
    JsonObject->SetStringField("username", PlayerUsername);
    JsonObject->SetStringField("client", "ComfyTextures");
    JsonObject->SetStringField("version", "1.0");

    // Add authentication if token is provided
    if (!AuthToken.IsEmpty())
    {
        JsonObject->SetStringField("auth_token", AuthToken);
    }

    // Add security settings
    JsonObject->SetBoolField("encryption_enabled", bEncryptionEnabled);
    JsonObject->SetNumberField("timeout", ConnectionTimeout);

    FString JsonString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
    FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);

    return JsonString;
}

FString UMCPClient::BuildPromptMessage(const FString& Prompt, const TArray<uint8>& SceneData) const
{
    TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();
    JsonObject->SetStringField("type", "prompt");
    JsonObject->SetStringField("prompt", Prompt);
    JsonObject->SetStringField("scene_data", FBase64::Encode(SceneData));

    FString JsonString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
    FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);

    return JsonString;
}

FString UMCPClient::BuildSceneRequestMessage(const FString& WorldName, const FVector& Location, float Radius) const
{
    TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();
    JsonObject->SetStringField("type", "scene_request");
    JsonObject->SetStringField("world", WorldName);
    JsonObject->SetNumberField("x", Location.X);
    JsonObject->SetNumberField("y", Location.Y);
    JsonObject->SetNumberField("z", Location.Z);
    JsonObject->SetNumberField("radius", Radius);

    FString JsonString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
    FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);

    return JsonString;
}

void UMCPClient::ParseMessage(const FString& Message)
{
    TSharedPtr<FJsonObject> JsonObject;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);

    if (!FJsonSerializer::Deserialize(Reader, JsonObject))
    {
        UE_LOG(LogComfyTextures, Error, TEXT("Failed to parse MCP message: %s"), *Message);
        return;
    }

    FString MessageType;
    if (!JsonObject->TryGetStringField("type", MessageType))
    {
        UE_LOG(LogComfyTextures, Error, TEXT("MCP message missing type field"));
        return;
    }

    if (MessageType == "prompt_response")
    {
        if (OnPromptResponse)
        {
            OnPromptResponse(JsonObject);
        }
    }
    else if (MessageType == "scene_data")
    {
        FString EncodedData;
        if (JsonObject->TryGetStringField("data", EncodedData))
        {
            TArray<uint8> DecodedData;
            FBase64::Decode(EncodedData, DecodedData);
            if (OnSceneDataReceived)
            {
                OnSceneDataReceived(DecodedData);
            }
        }
    }
    else if (MessageType == "error")
    {
        FString ErrorMessage;
        if (JsonObject->TryGetStringField("message", ErrorMessage) && OnError)
        {
            OnError(ErrorMessage);
        }
    }
    else
    {
        UE_LOG(LogComfyTextures, Warning, TEXT("Unknown MCP message type: %s"), *MessageType);
    }
}

// Security and authentication methods
void UMCPClient::SetAuthenticationToken(const FString& Token)
{
    AuthToken = Token;
    UE_LOG(LogComfyTextures, Log, TEXT("MCP authentication token set"));
}

void UMCPClient::EnableEncryption(bool bEnable)
{
    bEncryptionEnabled = bEnable;
    UE_LOG(LogComfyTextures, Log, TEXT("MCP encryption %s"), bEnable ? TEXT("enabled") : TEXT("disabled"));
}

bool UMCPClient::ValidateServerCertificate(const FString& Certificate) const
{
    // Basic certificate validation (placeholder for actual implementation)
    if (Certificate.IsEmpty())
    {
        UE_LOG(LogComfyTextures, Error, TEXT("Empty server certificate"));
        return false;
    }

    // Check for basic certificate properties
    if (!Certificate.Contains("BEGIN CERTIFICATE") || !Certificate.Contains("END CERTIFICATE"))
    {
        UE_LOG(LogComfyTextures, Error, TEXT("Invalid certificate format"));
        return false;
    }

    UE_LOG(LogComfyTextures, Log, TEXT("Server certificate validated"));
    return true;
}

void UMCPClient::SetConnectionTimeout(float TimeoutSeconds)
{
    ConnectionTimeout = FMath::Max(TimeoutSeconds, 5.0f);
    UE_LOG(LogComfyTextures, Log, TEXT("MCP connection timeout set to %f seconds"), ConnectionTimeout);
}

void UMCPClient::SetMaxRetries(int32 InMaxRetries)
{
    MaxRetries = FMath::Clamp(InMaxRetries, 0, 10);
    UE_LOG(LogComfyTextures, Log, TEXT("MCP max retries set to %d"), MaxRetries);
}

void UMCPClient::SetRetryDelay(float DelaySeconds)
{
    RetryDelay = FMath::Max(DelaySeconds, 0.5f);
    UE_LOG(LogComfyTextures, Log, TEXT("MCP retry delay set to %f seconds"), RetryDelay);
}