// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "IWebSocket.h"

/**
 * 
 */
class ComfyTexturesHttpClient
{
public:
	ComfyTexturesHttpClient(const FString& Url);

	void SetWebSocketStateChangedCallback(TFunction<void(bool)> Callback);

	void SetWebSocketMessageCallback(TFunction<void(const TSharedPtr<FJsonObject>&)> Callback);

	void Connect();

	bool IsConnected() const;

	bool DoHttpGetRequest(const FString& Url, TFunction<void(const TSharedPtr<FJsonObject>&, bool)> Callback) const;

	bool DoHttpGetRequestRaw(const FString& Url, TFunction<void(const TArray<uint8>&, bool)> Callback) const;

	bool DoHttpPostRequest(const FString& Url, const TSharedPtr<FJsonObject>& Payload, TFunction<void(const TSharedPtr<FJsonObject>&, bool)> Callback) const;

	bool DoHttpFileUpload(const FString& Url, const TArray64<uint8>& FileData, const FString& FileName, TFunction<void(const TSharedPtr<FJsonObject>&, bool)> Callback) const;
	
	const FString ClientId;

private:
	const FString BaseUrl;

	TFunction<void(bool)> OnWebSocketStateChanged;

	TFunction<void(const TSharedPtr<FJsonObject>&)> OnWebSocketMessage;

	// comfyui websocket connection
	TSharedPtr<IWebSocket> WebSocket;
};
