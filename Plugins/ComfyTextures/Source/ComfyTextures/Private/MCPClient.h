// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "IWebSocket.h"
#include "MCPClient.generated.h"

/**
 * MCP (Minecraft Protocol) Client for cross-platform communication
 * Handles communication between Unreal Engine and Minecraft servers
 */
UCLASS()
class COMFYTEXTURES_API UMCPClient : public UObject
{
    GENERATED_BODY()

public:
    UMCPClient();

    /** Initialize the MCP client with server details */
    void Initialize(const FString& ServerAddress, int32 Port, const FString& Username);

    /** Connect to Minecraft server */
    bool Connect();

    /** Disconnect from server */
    void Disconnect();

    /** Check if connected */
    bool IsConnected() const;

    /** Send a prompt for texture generation */
    bool SendPrompt(const FString& Prompt, const TArray<uint8>& SceneData);

    /** Request scene data from Minecraft world */
    bool RequestSceneData(const FString& WorldName, const FVector& Location, float Radius);

    /** Set callbacks for various events */
    void SetConnectionCallback(TFunction<void(bool)> Callback);
    void SetPromptResponseCallback(TFunction<void(const TSharedPtr<FJsonObject>&)> Callback);
    void SetSceneDataCallback(TFunction<void(const TArray<uint8>&)> Callback);
    void SetErrorCallback(TFunction<void(const FString&)> Callback);

    /** Security and authentication */
    void SetAuthenticationToken(const FString& Token);
    void EnableEncryption(bool bEnable);
    bool ValidateServerCertificate(const FString& Certificate) const;

    /** Connection security settings */
    void SetConnectionTimeout(float TimeoutSeconds);
    void SetMaxRetries(int32 MaxRetries);
    void SetRetryDelay(float DelaySeconds);

private:
    /** Server connection details */
    FString ServerAddress;
    int32 ServerPort;
    FString PlayerUsername;

    /** Connection state */
    bool bIsConnected;

    /** WebSocket connection for MCP communication */
    TSharedPtr<IWebSocket> WebSocket;

    /** Security settings */
    FString AuthToken;
    bool bEncryptionEnabled;
    float ConnectionTimeout;
    int32 MaxRetries;
    float RetryDelay;
    int32 CurrentRetryCount;

    /** Callbacks */
    TFunction<void(bool)> OnConnectionChanged;
    TFunction<void(const TSharedPtr<FJsonObject>&)> OnPromptResponse;
    TFunction<void(const TArray<uint8>&)> OnSceneDataReceived;
    TFunction<void(const FString&)> OnError;

    /** Handle WebSocket messages */
    void HandleWebSocketMessage(const FString& Message);
    void HandleWebSocketConnection(bool bConnected);
    void HandleWebSocketError(const FString& Error);

    /** MCP protocol message builders */
    FString BuildHandshakeMessage() const;
    FString BuildPromptMessage(const FString& Prompt, const TArray<uint8>& SceneData) const;
    FString BuildSceneRequestMessage(const FString& WorldName, const FVector& Location, float Radius) const;

    /** Parse incoming MCP messages */
    void ParseMessage(const FString& Message);
};