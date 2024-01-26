// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "IWebSocket.h"
#include "EditorUtilityWidget.h"
#include "Camera/CameraActor.h"
#include "ComfyTexturesWidgetBase.generated.h"

UENUM(BlueprintType)
enum class EComfyTexturesState : uint8
{
  Disconnected,
  Reconnecting,
  Idle,
  Rendering,
  Processing
};

UENUM(BlueprintType)
enum class EComfyTexturesRenderState : uint8
{
  Pending,
  Started,
  Finished,
  Failed
};

UENUM(BlueprintType)
enum class EComfyTexturesMode : uint8
{
  Create,
  Edit,
  Refine
};

UENUM(BlueprintType)
enum class EComfyTexturesEditMaskMode : uint8
{
  FromTexture,
  FromObject
};

UENUM(BlueprintType)
enum class EComfyTexturesRenderTextureMode : uint8
{
  Depth,
  RawDepth,
  Normals,
  Color
};

UENUM(BlueprintType)
enum class EComfyTexturesCameraMode : uint8
{
  EditorCamera,
  ExistingCamera,
  EightSides,
  Orbit,
};

UCLASS(config = Game, defaultconfig)
class UComfyTexturesSettings : public UObject
{
  GENERATED_BODY()

  public:
  UPROPERTY(EditAnywhere, config, Category = "General")
  FString ComfyUrl = "http://127.0.0.1:8188";

  UPROPERTY(EditAnywhere, config, Category = "General")
  int MinTextureSize = 64;

  UPROPERTY(EditAnywhere, config, Category = "General")
  int MaxTextureSize = 4096;

  UPROPERTY(EditAnywhere, config, Category = "General")
  float TextureQualityMultiplier = 0.5f;
};

USTRUCT(BlueprintType)
struct FComfyTexturesImageData
{
  GENERATED_BODY()

  public:
  UPROPERTY(BlueprintReadOnly)
  TArray<FLinearColor> Pixels;

  UPROPERTY(BlueprintReadOnly)
  int Width;

  UPROPERTY(BlueprintReadOnly)
  int Height;
};

USTRUCT(BlueprintType)
struct FComfyTexturesRenderData
{
  GENERATED_BODY()

  public:
  UPROPERTY(BlueprintReadOnly)
  FString PromptId;

  UPROPERTY(BlueprintReadOnly)
  EComfyTexturesRenderState State;

  UPROPERTY(BlueprintReadOnly)
  TArray<FString> OutputFileNames;

  UPROPERTY(BlueprintReadOnly)
  float Progress;

  UPROPERTY(BlueprintReadOnly)
  int CurrentNodeIndex;

  FMinimalViewInfo ViewInfo;

  FMatrix ViewMatrix;

  FMatrix ProjectionMatrix;

  TArray<FColor> OutputPixels;

  FComfyTexturesImageData RawDepth;

  int OutputWidth;

  int OutputHeight;
};

USTRUCT(BlueprintType)
struct FComfyTexturesWorkflowParams
{
  GENERATED_BODY()

  public:
  UPROPERTY(BlueprintReadWrite)
  FString PositivePrompt;

  UPROPERTY(BlueprintReadWrite)
  FString NegativePrompt;

  UPROPERTY(BlueprintReadWrite)
  int Seed;

  UPROPERTY(BlueprintReadWrite)
  float Cfg;

  UPROPERTY(BlueprintReadWrite)
  int Steps;

  UPROPERTY(BlueprintReadWrite)
  int RefinerSteps;

  UPROPERTY(BlueprintReadWrite)
  float DenoiseStrength;

  UPROPERTY(BlueprintReadWrite)
  float ControlDepthStrength;

  UPROPERTY(BlueprintReadWrite)
  float ControlCannyStrength;
};

USTRUCT(BlueprintType)
struct FComfyTexturesRenderOptions
{
  GENERATED_BODY()

  public:
  UPROPERTY(BlueprintReadWrite)
  EComfyTexturesMode Mode;

  UPROPERTY(BlueprintReadWrite)
  FString WorkflowJsonPath;

  UPROPERTY(BlueprintReadWrite)
  EComfyTexturesEditMaskMode EditMaskMode;

  UPROPERTY(BlueprintReadWrite)
  FComfyTexturesWorkflowParams Params;

  FString DepthImageFilename;

  FString NormalsImageFilename;

  FString ColorImageFilename;

  FString MaskImageFilename;

  FString EdgeMaskImageFilename;
};

USTRUCT(BlueprintType)
struct FComfyTexturesCaptureOutput
{
  GENERATED_BODY()

  public:
  UPROPERTY(BlueprintReadOnly)
  FComfyTexturesImageData RawDepth;

  UPROPERTY(BlueprintReadOnly)
  FComfyTexturesImageData Depth;

  UPROPERTY(BlueprintReadOnly)
  FComfyTexturesImageData Normals;

  UPROPERTY(BlueprintReadOnly)
  FComfyTexturesImageData Color;

  UPROPERTY(BlueprintReadOnly)
  FComfyTexturesImageData EditMask;

  UPROPERTY(BlueprintReadOnly)
  FComfyTexturesImageData EdgeMask;
};

USTRUCT(BlueprintType)
struct FComfyTexturesCameraOptions
{
  GENERATED_BODY()

  public:
  UPROPERTY(BlueprintReadWrite)
  EComfyTexturesCameraMode CameraMode;

  UPROPERTY(BlueprintReadWrite)
  ACameraActor* ExistingCamera;

  UPROPERTY(BlueprintReadWrite)
  int OrbitSteps;

  UPROPERTY(BlueprintReadWrite)
  float OrbitHeight;

  UPROPERTY(BlueprintReadWrite)
  float CameraFov;
};

USTRUCT(BlueprintType)
struct FComfyTexturesPrepareOptions
{
  GENERATED_BODY()

  public:
  UPROPERTY(BlueprintReadWrite)
  UMaterial* BaseMaterial;

  UPROPERTY(BlueprintReadWrite)
  UTexture2D* ReferenceTexture;

  UPROPERTY(BlueprintReadWrite)
  FMinimalViewInfo ViewInfo;
};

/**
 *
 */
UCLASS()
class COMFYTEXTURES_API UComfyTexturesWidgetBase : public UEditorUtilityWidget
{
  GENERATED_BODY()

  public:
  UFUNCTION(BlueprintImplementableEvent, Category = "ComfyTextures")
  void OnStateChanged(EComfyTexturesState NewState);

  UFUNCTION(BlueprintImplementableEvent, Category = "ComfyTextures")
  void OnRenderStateChanged(const FString& PromptId, const FComfyTexturesRenderData& Data);

  UPROPERTY(BlueprintReadOnly, Category = "ComfyTextures")
  EComfyTexturesState State = EComfyTexturesState::Disconnected;

  UPROPERTY(BlueprintReadOnly, Category = "ComfyTextures")
  FString ClientId;

  virtual bool Initialize() override;

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  void Connect();

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  bool IsConnected() const;

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  int GetNumPendingRequests() const;

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  bool HasPendingRequests() const;

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  bool ValidateAllRequestsSucceeded() const;

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  bool ProcessMultipleActors(const TArray<AActor*>& Actors, const FComfyTexturesRenderOptions& RenderOpts, const FComfyTexturesCameraOptions& CameraOpts);

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  bool ProcessRenderResults();

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  void CancelJob();

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  bool QueueRender(const FComfyTexturesRenderOptions& RenderOpts, int& RequestIndex);

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  void InterruptRender() const;

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  void ClearRenderQueue();

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  void FreeComfyMemory(bool bUnloadModels);

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  bool ReadTextFile(FString FilePath, FString& OutText) const;

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  bool PrepareActors(const TArray<AActor*>& Actor, const FComfyTexturesPrepareOptions& PrepareOpts);

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  bool ParseWorkflowJson(const FString& JsonPath, FComfyTexturesWorkflowParams& OutParams);

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  FString GetWorkflowJsonPath(EComfyTexturesMode Mode) const;

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  void SetEditorFrameRate(int Fps);

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  void GetFlattenedSelectionSetWithChildren(TArray<AActor*>& OutActors) const;

  protected:
  // comfyui websocket connection
  TSharedPtr<IWebSocket> WebSocket;

  // data for all render requests
  TMap<int, FComfyTexturesRenderData> RenderData;

  // request index to prompt id mapping
  TMap<FString, int> PromptIdToRequestIndex;

  // next request index to use
  int NextRequestIndex = 0;

  // actors that are currently being processed
  TArray<AActor*> ActorSet;

  private:
  void HandleRenderStateChanged(const FComfyTexturesRenderData& Data);

  FString GetBaseUrl() const;

  void HandleWebSocketMessage(const FString& Message);

  bool DoHttpGetRequest(const FString& Url, TFunction<void(const TSharedPtr<FJsonObject>&, bool)> Callback) const;

  bool DoHttpGetRequestRaw(const FString& Url, TFunction<void(const TArray<uint8>&, bool)> Callback) const;

  bool DoHttpPostRequest(const FString& Url, const FString& Content, TFunction<void(const TSharedPtr<FJsonObject>&, bool)> Callback) const;

  bool DoHttpFileUpload(const FString& Url, const TArray<uint8>& FileData, const FString& FileName, TFunction<void(const TSharedPtr<FJsonObject>&, bool)> Callback) const;

  bool CreateCameraTransforms(AActor* Actor, const FComfyTexturesCameraOptions& CameraOptions, TArray<FMinimalViewInfo>& OutViewInfos) const;

  bool CaptureSceneTextures(UWorld* World, TArray<AActor*> Actors, const TArray<FMinimalViewInfo>& ViewInfos, EComfyTexturesMode Mode, TArray<FComfyTexturesCaptureOutput>& Outputs) const;

  bool ReadRenderTargetPixels(UTextureRenderTarget2D* InputTexture, EComfyTexturesRenderTextureMode Mode, FComfyTexturesImageData& OutImage) const;

  bool SaveImageToPng(const FComfyTexturesImageData& Image, const FString& FilePath) const;

  bool ConvertImageToPng(const FComfyTexturesImageData& Image, TArray<uint8>& OutBytes) const;

  bool UploadImages(const TArray<FComfyTexturesImageData>& Images, const TArray<FString>& FileNames, TFunction<void(const TArray<FString>&, bool)> Callback) const;

  bool DownloadImage(const FString& FileName, TFunction<void(TArray<FColor>, int, int, bool)> Callback) const;

  bool ReadPngPixels(FString FilePath, TArray<FColor>& OutPixels, int& OutWidth, int& OutHeight) const;

  bool GenerateMipMaps(UTexture2D* Texture) const;

  bool CalculateApproximateScreenBounds(AActor* Actor, const FMinimalViewInfo& ViewInfo, FBox2D& OutBounds) const;

  UTexture2D* CreateTexture2D(int Width, int Height, const TArray<FColor>& Pixels) const;

  bool CreateAssetPackage(UObject* Asset, FString PackagePath) const;

  void CreateEditMaskFromImage(const TArray<FLinearColor>& Pixels, TArray<FLinearColor>& OutPixels) const;

  void CreateEdgeMask(const FComfyTexturesImageData& Depth, const FComfyTexturesImageData& Normals, FComfyTexturesImageData& OutEdgeMask) const;

  float ComputeDepthGradient(const FComfyTexturesImageData& Image, int X, int Y) const;

  float ComputeNormalsGradient(const FComfyTexturesImageData& Image, int X, int Y) const;

  void LoadRenderResultImages(TFunction<void(bool)> Callback);

  void TransitionToIdleState();
};
