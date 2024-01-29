// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EditorUtilityWidget.h"
#include "Camera/CameraActor.h"
#include "ComfyTexturesHttpClient.h"
#include "ComfyTexturesWidgetBase.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogComfyTextures, Log, All);

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
  ExistingCamera
};

UCLASS(config = Game, defaultconfig)
class UComfyTexturesSettings : public UObject
{
  GENERATED_BODY()

  public:
  UPROPERTY(EditAnywhere, config, Category = "General", meta = (DisplayName = "ComfyUI URL", ToolTip = "URL of your ComfyUI server, leave as is if running locally"))
  FString ComfyUrl = "http://127.0.0.1:8188";

  UPROPERTY(EditAnywhere, config, Category = "General", meta = (DisplayName = "Limit Editor FPS", ToolTip = "Limit the editor frames per second while rendering"))
  bool bLimitEditorFps = true;

  UPROPERTY(EditAnywhere, config, Category = "General", meta = (DisplayName = "Min. Texture Size", ToolTip = "Minimum texture size for generated textures, should be a power of 2"))
  int MinTextureSize = 64;

  UPROPERTY(EditAnywhere, config, Category = "General", meta = (DisplayName = "Max. Texture Size", ToolTip = "Maximum texture size for generated textures, should be a power of 2"))
  int MaxTextureSize = 4096;

  UPROPERTY(EditAnywhere, config, Category = "General", meta = (DisplayName = "Texture Quality Multiplier", ToolTip = "Multiplier for texture quality, higher is better"))
  float TextureQualityMultiplier = 0.5f;

  UPROPERTY(EditAnywhere, config, Category = "General", meta = (DisplayName = "Capture Size", ToolTip = "Size of the scene capture textures, should be a power of 2"))
  int CaptureSize = 2048;

  UPROPERTY(EditAnywhere, config, Category = "General", meta = (DisplayName = "Upload Size", ToolTip = "Size of images uploaded to ComfyUI as workflow inputs"))
  int UploadSize = 1024;
};

USTRUCT(BlueprintType)
struct FComfyTexturesImageData
{
  GENERATED_BODY()

  UPROPERTY(BlueprintReadOnly)
  TArray<FLinearColor> Pixels;

  UPROPERTY(BlueprintReadOnly)
  int Width = 0;

  UPROPERTY(BlueprintReadOnly)
  int Height = 0;
};

USTRUCT(BlueprintType)
struct FComfyTexturesRenderData
{
  GENERATED_BODY()

  UPROPERTY(BlueprintReadOnly)
  FString PromptId;

  UPROPERTY(BlueprintReadOnly)
  EComfyTexturesRenderState State = EComfyTexturesRenderState::Pending;

  UPROPERTY(BlueprintReadOnly)
  TArray<FString> OutputFileNames;

  UPROPERTY(BlueprintReadOnly)
  float Progress = 0.0f;

  UPROPERTY(BlueprintReadOnly)
  int CurrentNodeIndex = -1;

  FMinimalViewInfo ViewInfo;

  FMatrix ViewMatrix;

  FMatrix ProjectionMatrix;

  TArray<FColor> OutputPixels;

  FComfyTexturesImageData RawDepth;

  int OutputWidth = 0;

  int OutputHeight = 0;
  
  bool bPreserveExisting = false;

  float PreserveThreshold = 0.5f;
};

USTRUCT(BlueprintType)
struct FComfyTexturesWorkflowParams
{
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite)
  FString PositivePrompt;

  UPROPERTY(BlueprintReadWrite)
  FString NegativePrompt;

  UPROPERTY(BlueprintReadWrite)
  int Seed = 0;

  UPROPERTY(BlueprintReadWrite)
  float Cfg = 8.0f;

  UPROPERTY(BlueprintReadWrite)
  int Steps = 10;

  UPROPERTY(BlueprintReadWrite)
  int RefinerSteps = 5;

  UPROPERTY(BlueprintReadWrite)
  float DenoiseStrength = 0.9f;

  UPROPERTY(BlueprintReadWrite)
  float ControlDepthStrength = 0.3f;

  UPROPERTY(BlueprintReadWrite)
  float ControlCannyStrength = 0.3f;

  UPROPERTY(BlueprintReadWrite)
  EComfyTexturesEditMaskMode EditMaskMode = EComfyTexturesEditMaskMode::FromObject;
};

USTRUCT(BlueprintType)
struct FComfyTexturesRenderOptions
{
  GENERATED_BODY()

  UPROPERTY(BlueprintReadWrite)
  EComfyTexturesMode Mode;

  UPROPERTY(BlueprintReadWrite)
  FComfyTexturesWorkflowParams Params;

  UPROPERTY(BlueprintReadWrite)
  EComfyTexturesCameraMode CameraMode;

  UPROPERTY(BlueprintReadWrite)
  ACameraActor* ExistingCamera;

  UPROPERTY(BlueprintReadWrite)
  bool bPreserveExisting;

  UPROPERTY(BlueprintReadWrite)
  float PreserveThreshold;

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
struct FComfyTexturesPrepareOptions
{
  GENERATED_BODY()

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
  using FComfyTexturesRenderDataPtr = TSharedPtr<FComfyTexturesRenderData>;

  UPROPERTY(BlueprintReadOnly, Category = "ComfyTextures")
  EComfyTexturesState State = EComfyTexturesState::Disconnected;

  UFUNCTION(BlueprintImplementableEvent, Category = "ComfyTextures")
  void OnStateChanged(EComfyTexturesState NewState);

  UFUNCTION(BlueprintImplementableEvent, Category = "ComfyTextures")
  void OnRenderStateChanged(const FString& PromptId, const FComfyTexturesRenderData& Data);

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
  bool ProcessMultipleActors(const TArray<AActor*>& Actors, const FComfyTexturesRenderOptions& RenderOpts);

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
  bool PrepareActors(const TArray<AActor*>& Actor, const FComfyTexturesPrepareOptions& PrepareOpts);

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  bool ParseWorkflowJson(const FString& JsonPath, FComfyTexturesWorkflowParams& OutParams);

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  FString GetWorkflowJsonPath(EComfyTexturesMode Mode) const;

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  void SetEditorFrameRate(int Fps);

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  void GetFlattenedSelectionSetWithChildren(TArray<AActor*>& OutActors) const;

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  bool LoadParams();

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  bool SaveParams();

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  void SetParams(EComfyTexturesMode Mode, const FComfyTexturesWorkflowParams& InParams);

  UFUNCTION(BlueprintCallable, Category = "ComfyTextures")
  void GetParams(EComfyTexturesMode Mode, FComfyTexturesWorkflowParams& OutParams) const;

  protected:
  TUniquePtr<ComfyTexturesHttpClient> HttpClient;

  // data for all render requests
  TMap<int, FComfyTexturesRenderDataPtr> RenderQueue;

  // request index to prompt id mapping
  TMap<FString, int> PromptIdToRequestIndex;

  // next request index to use
  int NextRequestIndex = 0;

  // actors that are currently being processed
  TArray<AActor*> ActorSet;

  // user-selected workflow parameters for each mode
  TMap<EComfyTexturesMode, FComfyTexturesWorkflowParams> Params;

  private:
  FString GetBaseUrl() const;

  bool ProcessRenderResultForActor(AActor* Actor, TFunction<void(bool)> Callback);

  void HandleRenderStateChanged(const FComfyTexturesRenderData& Data);

  void HandleWebSocketMessage(const TSharedPtr<FJsonObject>& Message);

  bool CreateCameraTransforms(AActor* Actor, const FComfyTexturesRenderOptions& RenderOpts, TArray<FMinimalViewInfo>& OutViewInfos) const;

  bool CaptureSceneTextures(UWorld* World, TArray<AActor*> Actors, const TArray<FMinimalViewInfo>& ViewInfos, EComfyTexturesMode Mode, const TSharedPtr<TArray<FComfyTexturesCaptureOutput>>& Outputs) const;

  void ProcessSceneTextures(const TSharedPtr<TArray<FComfyTexturesCaptureOutput>>& Outputs, EComfyTexturesMode Mode, int TargetSize, TFunction<void()> Callback) const;

  bool ReadRenderTargetPixels(UTextureRenderTarget2D* InputTexture, EComfyTexturesRenderTextureMode Mode, FComfyTexturesImageData& OutImage) const;

  bool ConvertImageToPng(const FComfyTexturesImageData& Image, TArray64<uint8>& OutBytes) const;

  bool UploadImages(const TArray<FComfyTexturesImageData>& Images, const TArray<FString>& FileNames, TFunction<void(const TArray<FString>&, bool)> Callback) const;

  bool DownloadImage(const FString& FileName, TFunction<void(TArray<FColor>, int, int, bool)> Callback) const;

  bool CalculateApproximateScreenBounds(AActor* Actor, const FMinimalViewInfo& ViewInfo, FBox2D& OutBounds) const;

  UTexture2D* CreateTexture2D(int Width, int Height, const TArray<FColor>& Pixels) const;

  bool CreateAssetPackage(UObject* Asset, FString PackagePath) const;

  void CreateEditMaskFromImage(const TArray<FLinearColor>& Pixels, TArray<FLinearColor>& OutPixels) const;

  void CreateEdgeMask(const FComfyTexturesImageData& Depth, const FComfyTexturesImageData& Normals, FComfyTexturesImageData& OutEdgeMask) const;

  void LoadRenderResultImages(TFunction<void(bool)> Callback);

  void TransitionToIdleState();

  void ResizeImage(FComfyTexturesImageData& Image, int NewWidth, int NewHeight) const;
};
