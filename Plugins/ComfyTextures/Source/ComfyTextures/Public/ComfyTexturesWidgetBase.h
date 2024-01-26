// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EditorUtilityWidget.h"
#include "Camera/CameraActor.h"
#include "ComfyTexturesHttpClient.h"
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

  UPROPERTY(BlueprintReadWrite)
  FString PositivePrompt;

  UPROPERTY(BlueprintReadWrite)
  FString NegativePrompt;

  UPROPERTY(BlueprintReadWrite)
  int Seed = 0;

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

  UPROPERTY(BlueprintReadWrite)
  EComfyTexturesEditMaskMode EditMaskMode;
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
  int OrbitSteps;

  UPROPERTY(BlueprintReadWrite)
  float OrbitHeight;

  UPROPERTY(BlueprintReadWrite)
  float CameraFov;

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
  TMap<int, FComfyTexturesRenderData> RenderData;

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

  void HandleRenderStateChanged(const FComfyTexturesRenderData& Data);

  void HandleWebSocketMessage(const TSharedPtr<FJsonObject>& Message);

  bool CreateCameraTransforms(AActor* Actor, const FComfyTexturesRenderOptions& RenderOpts, TArray<FMinimalViewInfo>& OutViewInfos) const;

  bool CaptureSceneTextures(UWorld* World, TArray<AActor*> Actors, const TArray<FMinimalViewInfo>& ViewInfos, EComfyTexturesMode Mode, TArray<FComfyTexturesCaptureOutput>& Outputs) const;

  bool ReadRenderTargetPixels(UTextureRenderTarget2D* InputTexture, EComfyTexturesRenderTextureMode Mode, FComfyTexturesImageData& OutImage) const;

  bool ConvertImageToPng(const FComfyTexturesImageData& Image, TArray64<uint8>& OutBytes) const;

  bool UploadImages(const TArray<FComfyTexturesImageData>& Images, const TArray<FString>& FileNames, TFunction<void(const TArray<FString>&, bool)> Callback) const;

  bool DownloadImage(const FString& FileName, TFunction<void(TArray<FColor>, int, int, bool)> Callback) const;

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
