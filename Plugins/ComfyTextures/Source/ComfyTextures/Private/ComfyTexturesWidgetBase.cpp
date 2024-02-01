// Fill out your copyright notice in the Description page of Project Settings.


#include "ComfyTexturesWidgetBase.h"
#include "Kismet/GameplayStatics.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Camera/CameraComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Serialization/JsonSerializer.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "EditorAssetLibrary.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageUtils.h"
#include "FileHelpers.h"
#include "UObject/SavePackage.h"
#include "ScopedTransaction.h"
#include "Engine/Selection.h"

#define LOCTEXT_NAMESPACE "ComfyTextures"

DEFINE_LOG_CATEGORY(LogComfyTextures);

void UComfyTexturesWidgetBase::Connect()
{
  if (!HttpClient.IsValid())
  {
    HttpClient = MakeUnique<ComfyTexturesHttpClient>(GetBaseUrl());
  }

  TWeakObjectPtr<UComfyTexturesWidgetBase> WeakThis(this);

  HttpClient->SetWebSocketStateChangedCallback([WeakThis](bool bConnected)
    {
      if (!WeakThis.IsValid())
      {
        return;
      }

      UComfyTexturesWidgetBase* This = WeakThis.Get();

      if (bConnected)
      {
        This->TransitionToIdleState();
      }
      else
      {
        This->State = EComfyTexturesState::Disconnected;
        This->OnStateChanged(This->State);
      }
    });

  HttpClient->SetWebSocketMessageCallback([WeakThis](const TSharedPtr<FJsonObject>& Message)
    {
      if (!WeakThis.IsValid())
      {
        return;
      }

      UComfyTexturesWidgetBase* This = WeakThis.Get();

      This->HandleWebSocketMessage(Message);
    });

  HttpClient->Connect();

  State = EComfyTexturesState::Reconnecting;
  OnStateChanged(State);
}

bool UComfyTexturesWidgetBase::IsConnected() const
{
  if (!HttpClient.IsValid())
  {
    return false;
  }

  return HttpClient->IsConnected();
}

int UComfyTexturesWidgetBase::GetNumPendingRequests() const
{
  int NumPendingRequests = 0;

  for (const TPair<int, FComfyTexturesRenderDataPtr>& Pair : RenderQueue)
  {
    if (Pair.Value->State == EComfyTexturesRenderState::Pending)
    {
      NumPendingRequests++;
    }
  }

  return NumPendingRequests;
}

bool UComfyTexturesWidgetBase::HasPendingRequests() const
{
  return GetNumPendingRequests() > 0;
}

bool UComfyTexturesWidgetBase::ValidateAllRequestsSucceeded() const
{
  for (const TPair<int, FComfyTexturesRenderDataPtr>& Pair : RenderQueue)
  {
    if (Pair.Value->State != EComfyTexturesRenderState::Finished)
    {
      return false;
    }

    if (Pair.Value->OutputFileNames.Num() == 0)
    {
      return false;
    }
  }

  return true;
}

bool UComfyTexturesWidgetBase::ProcessMultipleActors(const TArray<AActor*>& Actors, const FComfyTexturesRenderOptions& RenderOpts)
{
  if (!IsConnected())
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Not connected to ComfyUI"));
    return false;
  }

  if (State != EComfyTexturesState::Idle)
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Not idle"));
    return false;
  }

  if (Actors.Num() == 0)
  {
    UE_LOG(LogComfyTextures, Error, TEXT("No actors to process"));
    return false;
  }

  State = EComfyTexturesState::Rendering;
  OnStateChanged(State);

  RenderQueue.Empty();
  PromptIdToRequestIndex.Empty();
  ActorSet = Actors;

  TArray<FMinimalViewInfo> ViewInfos;
  if (!CreateCameraTransforms(Actors[0], RenderOpts, ViewInfos))
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Failed to create camera transforms"));
    TransitionToIdleState();
    return false;
  }

  UTexture2D* MagentaPixel = nullptr;
  TMap<AActor*, TPair<UMaterialInstanceDynamic*, UTexture*>> ActorToTextureMap;

  if (RenderOpts.Mode == EComfyTexturesMode::Edit && RenderOpts.Params.EditMaskMode == EComfyTexturesEditMaskMode::FromObject)
  {
    MagentaPixel = UTexture2D::CreateTransient(1, 1, PF_B8G8R8A8);

    FTexturePlatformData* PlatformData = new FTexturePlatformData();
    PlatformData->SizeX = 1;
    PlatformData->SizeY = 1;
    PlatformData->PixelFormat = PF_B8G8R8A8;

    FTexture2DMipMap* Mip = new FTexture2DMipMap();
    Mip->SizeX = 1;
    Mip->SizeY = 1;
    Mip->BulkData.Lock(LOCK_READ_WRITE);
    Mip->BulkData.Realloc(4);
    Mip->BulkData.Unlock();

    FColor Magenta = FColor(255, 0, 255, 255);

    void* TextureData = Mip->BulkData.Lock(LOCK_READ_WRITE);
    FMemory::Memcpy(TextureData, &Magenta, 4);
    Mip->BulkData.Unlock();

    PlatformData->Mips.Add(Mip);

    MagentaPixel->SetPlatformData(PlatformData);
    MagentaPixel->UpdateResource();

    for (AActor* Actor : Actors)
    {
      // replace the material of the static mesh with the magenta pixel texture

      UStaticMeshComponent* StaticMeshComponent = Actor->FindComponentByClass<UStaticMeshComponent>();

      // if the actor doesn't have a static mesh component, skip it
      if (StaticMeshComponent == nullptr)
      {
        continue;
      }

      // get the material of the static mesh
      UMaterialInterface* Material = StaticMeshComponent->GetMaterial(0);

      // if the material is null, skip it

      if (Material == nullptr)
      {
        UE_LOG(LogComfyTextures, Warning, TEXT("Material is null for actor %s."), *Actor->GetName());
        continue;
      }

      // cast the material to a material instance dynamic
      UMaterialInstanceDynamic* MaterialInstance = Cast<UMaterialInstanceDynamic>(Material);

      // if the material is not a material instance dynamic, skip it
      if (MaterialInstance == nullptr)
      {
        UE_LOG(LogComfyTextures, Warning, TEXT("Material is not a material instance dynamic for actor %s."), *Actor->GetName());
        continue;
      }

      UTexture* OldTexture = nullptr;
      if (!MaterialInstance->GetTextureParameterValue(TEXT("BaseColor"), OldTexture))
      {
        UE_LOG(LogComfyTextures, Warning, TEXT("Failed to get parameter value \"BaseColor\" for actor %s."), *Actor->GetName());
        continue;
      }

      ActorToTextureMap.Add(Actor, TPair<UMaterialInstanceDynamic*, UTexture*>(MaterialInstance, OldTexture));

      // set the texture parameter to the magenta pixel texture
      MaterialInstance->SetTextureParameterValue(TEXT("BaseColor"), MagentaPixel);
    }
  }

  TSharedPtr<TArray<FComfyTexturesCaptureOutput>> CaptureResults = MakeShared<TArray<FComfyTexturesCaptureOutput>>();

  double CaptureSceneTexturesTime = 0.0;
  {
    SCOPE_SECONDS_COUNTER(CaptureSceneTexturesTime);
    if (!CaptureSceneTextures(Actors[0]->GetWorld(), Actors, ViewInfos, RenderOpts.Mode, CaptureResults))
    {
      UE_LOG(LogComfyTextures, Error, TEXT("Failed to capture input textures"));
      TransitionToIdleState();
      return false;
    }
  }

  UE_LOG(LogComfyTextures, Display, TEXT("Capture scene textures took %f seconds"), CaptureSceneTexturesTime);

  // bring back the original textures by iterating the actor to texture map

  for (TPair<AActor*, TPair<UMaterialInstanceDynamic*, UTexture*>>& Pair : ActorToTextureMap)
  {
    AActor* Actor = Pair.Key;
    TPair<UMaterialInstanceDynamic*, UTexture*>& Value = Pair.Value;

    UMaterialInstanceDynamic* MaterialInstance = Value.Key;
    UTexture* OldTexture = Value.Value;

    MaterialInstance->SetTextureParameterValue(TEXT("BaseColor"), OldTexture);
  }

  if (MagentaPixel != nullptr)
  {
    MagentaPixel->ConditionalBeginDestroy();
  }

  UComfyTexturesSettings* Settings = GetMutableDefault<UComfyTexturesSettings>();

  ProcessSceneTextures(CaptureResults, RenderOpts.Mode, Settings->UploadSize, [this, CaptureResults, ViewInfos, RenderOpts]()
    {
      for (int Index = 0; Index < CaptureResults->Num(); Index++)
      {
        const FComfyTexturesCaptureOutput& Output = (*CaptureResults)[Index];
        const FMinimalViewInfo& ViewInfo = ViewInfos[Index];
        const FComfyTexturesImageData& RawDepth = Output.RawDepth;

        TArray<FComfyTexturesImageData> Images;
        TArray<FString> FileNames;

        Images.Add(Output.Depth);
        FileNames.Add("depth_" + FString::FromInt(Index) + ".png");

        Images.Add(Output.Normals);
        FileNames.Add("normals_" + FString::FromInt(Index) + ".png");

        Images.Add(Output.Color);
        FileNames.Add("color_" + FString::FromInt(Index) + ".png");

        Images.Add(Output.EdgeMask);
        FileNames.Add("edge_mask_" + FString::FromInt(Index) + ".png");

        if (RenderOpts.Mode == EComfyTexturesMode::Edit)
        {
          Images.Add(Output.EditMask);
          FileNames.Add("mask_" + FString::FromInt(Index) + ".png");
        }

        bool bSuccess = UploadImages(Images, FileNames, [this, RenderOpts, ViewInfo, RawDepth](const TArray<FString>& FileNames, bool bSuccess)
          {
            if (!bSuccess)
            {
              UE_LOG(LogComfyTextures, Error, TEXT("Upload failed"));
              TransitionToIdleState();
              return;
            }

            UE_LOG(LogComfyTextures, Verbose, TEXT("Upload complete"));

            for (const FString& FileName : FileNames)
            {
              UE_LOG(LogComfyTextures, Verbose, TEXT("Uploaded file: %s"), *FileName);
            }

            FComfyTexturesRenderOptions NewRenderOpts = RenderOpts;
            NewRenderOpts.DepthImageFilename = FileNames[0];
            NewRenderOpts.NormalsImageFilename = FileNames[1];
            NewRenderOpts.ColorImageFilename = FileNames[2];
            NewRenderOpts.EdgeMaskImageFilename = FileNames[3];

            if (RenderOpts.Mode == EComfyTexturesMode::Edit)
            {
              NewRenderOpts.MaskImageFilename = FileNames[4];
            }

            if (State == EComfyTexturesState::Idle)
            {
              UE_LOG(LogComfyTextures, Warning, TEXT("State is idle"));
              return;
            }

            int RequestIndex;
            if (!QueueRender(NewRenderOpts, RequestIndex))
            {
              UE_LOG(LogComfyTextures, Error, TEXT("Failed to queue render"));
              TransitionToIdleState();
              return;
            }

            TOptional<FMatrix> CustomProjectionMatrix;
            FMatrix ViewMatrix, ProjectionMatrix, ViewProjectionMatrix;
            UGameplayStatics::CalculateViewProjectionMatricesFromMinimalView(ViewInfo, CustomProjectionMatrix,
              ViewMatrix, ProjectionMatrix, ViewProjectionMatrix);

            if (!RenderQueue.Contains(RequestIndex))
            {
              UE_LOG(LogComfyTextures, Error, TEXT("Render queue does not contain request index"));
              TransitionToIdleState();
              return;
            }

            const FComfyTexturesRenderDataPtr& Data = RenderQueue[RequestIndex];
            Data->ViewInfo = ViewInfo;
            Data->ViewMatrix = ViewMatrix;
            Data->ProjectionMatrix = ProjectionMatrix;
            Data->RawDepth = RawDepth;
            Data->bPreserveExisting = RenderOpts.bPreserveExisting;
            Data->PreserveThreshold = RenderOpts.PreserveThreshold;
          });

        if (!bSuccess)
        {
          UE_LOG(LogComfyTextures, Error, TEXT("Failed to upload capture results"));
          TransitionToIdleState();
        }
      }
    });

  return true;
}

static FLinearColor SampleBilinear(const TArray<FColor>& Pixels, int Width, int Height, FVector2D Uv)
{
  // clamp the UV coordinates
  Uv.X = FMath::Clamp(Uv.X, 0.0f, 1.0f);
  Uv.Y = FMath::Clamp(Uv.Y, 0.0f, 1.0f);

  // calculate the pixel coordinates
  float PixelX = Uv.X * (Width - 1);
  float PixelY = Uv.Y * (Height - 1);

  // calculate the pixel coordinates of the four surrounding pixels
  int PixelX0 = FMath::FloorToInt(PixelX);
  int PixelY0 = FMath::FloorToInt(PixelY);
  int PixelX1 = FMath::CeilToInt(PixelX);
  int PixelY1 = FMath::CeilToInt(PixelY);

  // calculate the pixel weights
  float WeightX = PixelX - PixelX0;
  float WeightY = PixelY - PixelY0;

  // sample the four surrounding pixels
  const FColor& Pixel00 = Pixels[PixelY0 * Width + PixelX0];
  const FColor& Pixel01 = Pixels[PixelY0 * Width + PixelX1];
  const FColor& Pixel10 = Pixels[PixelY1 * Width + PixelX0];
  const FColor& Pixel11 = Pixels[PixelY1 * Width + PixelX1];

  FLinearColor Pixel00L = FLinearColor(Pixel00.R, Pixel00.G, Pixel00.B, Pixel00.A) * (1.0f / 255.0f);
  FLinearColor Pixel01L = FLinearColor(Pixel01.R, Pixel01.G, Pixel01.B, Pixel01.A) * (1.0f / 255.0f);
  FLinearColor Pixel10L = FLinearColor(Pixel10.R, Pixel10.G, Pixel10.B, Pixel10.A) * (1.0f / 255.0f);
  FLinearColor Pixel11L = FLinearColor(Pixel11.R, Pixel11.G, Pixel11.B, Pixel11.A) * (1.0f / 255.0f);

  // interpolate the pixels
  FLinearColor Pixel0 = FMath::Lerp(Pixel00L, Pixel01L, WeightX);
  FLinearColor Pixel1 = FMath::Lerp(Pixel10L, Pixel11L, WeightX);
  FLinearColor Pixel = FMath::Lerp(Pixel0, Pixel1, WeightY);

  return Pixel;
}

static void ExpandTextureIslands(TArray<FColor>& Pixels, int Width, int Height, int Iterations)
{
  for (int Iteration = 0; Iteration < Iterations; Iteration++)
  {
    TArray<FColor> PixelsCopy = Pixels;

    for (int Y = 0; Y < Height; Y++)
    {
      for (int X = 0; X < Width; X++)
      {
        int Index = Y * Width + X;
        FColor Pixel = PixelsCopy[Index];
        if (Pixel.A > 0)
        {
          continue;
        }

        int NeighborCount = 0;
        FLinearColor NeighborColorSum(0.0f, 0.0f, 0.0f, 0.0f);

        // check the pixel's neighbors

        if (X > 0)
        {
          int NeighborIndex = Y * Width + (X - 1);
          FColor& NeighborPixel = PixelsCopy[NeighborIndex];
          if (NeighborPixel.A > 0)
          {
            NeighborCount++;
            NeighborColorSum += NeighborPixel.ReinterpretAsLinear();
          }
        }

        if (X < Width - 1)
        {
          int NeighborIndex = Y * Width + (X + 1);
          FColor& NeighborPixel = PixelsCopy[NeighborIndex];
          if (NeighborPixel.A > 0)
          {
            NeighborCount++;
            NeighborColorSum += NeighborPixel.ReinterpretAsLinear();
          }
        }

        if (Y > 0)
        {
          int NeighborIndex = (Y - 1) * Width + X;
          FColor& NeighborPixel = PixelsCopy[NeighborIndex];
          if (NeighborPixel.A > 0)
          {
            NeighborCount++;
            NeighborColorSum += NeighborPixel.ReinterpretAsLinear();
          }
        }

        if (Y < Height - 1)
        {
          int NeighborIndex = (Y + 1) * Width + X;
          FColor& NeighborPixel = PixelsCopy[NeighborIndex];
          if (NeighborPixel.A > 0)
          {
            NeighborCount++;
            NeighborColorSum += NeighborPixel.ReinterpretAsLinear();
          }
        }

        if (NeighborCount > 0)
        {
          Pixel.R = (NeighborColorSum.R / NeighborCount) * 255.0f;
          Pixel.G = (NeighborColorSum.G / NeighborCount) * 255.0f;
          Pixel.B = (NeighborColorSum.B / NeighborCount) * 255.0f;
          Pixel.A = 0;
          Pixels[Index] = Pixel;
        }
      }
    }
  }
}

static void RasterizeTriangle(FVector2D V0, FVector2D V1, FVector2D V2, int Width, int Height, TFunction<void(int, int, const FVector&)> Callback)
{
  FVector2D Size(Width - 1, Height - 1);
  V0 *= Size;
  V1 *= Size;
  V2 *= Size;

  // Bounding box
  int MinX = FMath::FloorToInt(FMath::Max(FMath::Min3(V0.X, V1.X, V2.X), 0));
  int MinY = FMath::FloorToInt(FMath::Max(FMath::Min3(V0.Y, V1.Y, V2.Y), 0));
  int MaxX = FMath::CeilToInt(FMath::Min(FMath::Max3(V0.X, V1.X, V2.X), Width - 1));
  int MaxY = FMath::CeilToInt(FMath::Min(FMath::Max3(V0.Y, V1.Y, V2.Y), Height - 1));

  for (int Y = MinY; Y <= MaxY; Y++)
  {
    for (int X = MinX; X <= MaxX; X++)
    {
      FVector Barycentric = FMath::GetBaryCentric2D(FVector2D(X, Y), V0, V1, V2);
      if (Barycentric.X < 0 || Barycentric.Y < 0 || Barycentric.Z < 0)
      {
        continue;
      }

      Callback(X, Y, Barycentric);
    }
  }
}

bool UComfyTexturesWidgetBase::ProcessRenderResultForActor(AActor* Actor, TFunction<void(bool)> Callback)
{
  const FTransform& ActorTransform = Actor->GetActorTransform();

  // get the static mesh component
  UStaticMeshComponent* StaticMeshComponent = Actor->FindComponentByClass<UStaticMeshComponent>();

  // if the actor doesn't have a static mesh component, skip it
  if (StaticMeshComponent == nullptr)
  {
    return false;
  }

  // get the static mesh
  UStaticMesh* StaticMesh = StaticMeshComponent->GetStaticMesh();

  // if the static mesh is null, skip it
  if (StaticMesh == nullptr)
  {
    UE_LOG(LogComfyTextures, Warning, TEXT("Static mesh is null for actor %s."), *Actor->GetName());
    return false;
  }

  // get the material of the static mesh
  UMaterialInterface* Material = StaticMeshComponent->GetMaterial(0);

  // if the material is null, skip it
  if (Material == nullptr)
  {
    UE_LOG(LogComfyTextures, Warning, TEXT("Material is null for actor %s."), *Actor->GetName());
    return false;
  }

  UTexture2D* Texture2D = nullptr;

  // get the texture assigned in the Tex param
  UTexture* Texture = nullptr;
  if (!Material->GetTextureParameterValue(TEXT("BaseColor"), Texture))
  {
    UE_LOG(LogComfyTextures, Warning, TEXT("Failed to get parameter value \"BaseColor\" for actor %s."), *Actor->GetName());
    return false;
  }

  // if the texture is null, skip it
  if (Texture == nullptr)
  {
    UE_LOG(LogComfyTextures, Warning, TEXT("Texture is null for actor %s."), *Actor->GetName());
    return false;
  }

  Texture2D = Cast<UTexture2D>(Texture);
  if (Texture2D == nullptr)
  {
    UE_LOG(LogComfyTextures, Warning, TEXT("Failed to create texture for actor %s."), *Actor->GetName());
    return false;
  }

  if (Texture2D->Source.GetNumMips() <= 0)
  {
    UE_LOG(LogComfyTextures, Warning, TEXT("No mipmaps available in texture for actor %s."), *Actor->GetName());
    return false;
  }

  int TextureWidth = Texture2D->GetSizeX();
  int TextureHeight = Texture2D->GetSizeY();

  const FStaticMeshLODResources& MeshLod = StaticMesh->GetLODForExport(0);

  struct SharedData
  {
    TArray<uint32> Indices;
    TArray<FVector> Vertices;
    TArray<FVector2D> Uvs;
    TSharedPtr<TArray<FColor>> Pixels;
    FComfyTexturesRenderDataPtr RenderData;
    FTransform ActorTransform;
    UTexture2D* Texture2D;
    AActor* Actor;
    int TextureWidth;
    int TextureHeight;
  };

  TSharedPtr<SharedData> StateData = MakeShared<SharedData>();
  StateData->RenderData = RenderQueue.begin().Value();
  StateData->ActorTransform = ActorTransform;
  StateData->TextureWidth = TextureWidth;
  StateData->TextureHeight = TextureHeight;
  StateData->Texture2D = Texture2D;
  StateData->Actor = Actor;
  StateData->Pixels = MakeShared<TArray<FColor>>();
  StateData->Pixels->SetNumZeroed(TextureWidth * TextureHeight);

  if (StateData->RenderData->bPreserveExisting)
  {
    FColor* MipData = (FColor*)Texture2D->Source.LockMip(0);
    if (MipData == nullptr)
    {
      UE_LOG(LogComfyTextures, Error, TEXT("Failed to lock mip 0 for texture %s."), *(Texture2D->GetName()));
      Callback(false);
      return false;
    }

    FMemory::Memcpy(StateData->Pixels->GetData(), MipData, StateData->Pixels->Num() * sizeof(FColor));
    Texture2D->Source.UnlockMip(0);
  }

  MeshLod.IndexBuffer.GetCopy(StateData->Indices);

  int VertexCount = MeshLod.VertexBuffers.PositionVertexBuffer.GetNumVertices();
  StateData->Vertices.SetNumUninitialized(VertexCount);
  StateData->Uvs.SetNumUninitialized(VertexCount);

  for (int32 VertexIndex = 0; VertexIndex < VertexCount; VertexIndex++)
  {
    FVector Vertex = (FVector)MeshLod.VertexBuffers.PositionVertexBuffer.VertexPosition(VertexIndex);
    StateData->Vertices[VertexIndex] = Vertex;

    FVector2D Uv = (FVector2D)MeshLod.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(VertexIndex, 0);
    StateData->Uvs[VertexIndex] = Uv;
  }

  AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [StateData, Callback]()
    {
      const FMinimalViewInfo& ViewInfo = StateData->RenderData->ViewInfo;
      const FMatrix& ViewMatrix = StateData->RenderData->ViewMatrix;
      const FMatrix& ProjectionMatrix = StateData->RenderData->ProjectionMatrix;

      FMatrix ViewProjectionMatrix = ViewMatrix * ProjectionMatrix;
      FIntRect ViewRect(0, 0, StateData->RenderData->OutputWidth, StateData->RenderData->OutputHeight);

      // Iterate over the faces
      for (int32 FaceIndex = 0; FaceIndex < StateData->Indices.Num(); FaceIndex += 3)
      {
        // Each face is represented by 3 indices
        uint32 Index0 = StateData->Indices[FaceIndex];
        uint32 Index1 = StateData->Indices[FaceIndex + 1];
        uint32 Index2 = StateData->Indices[FaceIndex + 2];

        // get the vertices of the face
        const FVector& Vertex0 = StateData->Vertices[Index0];
        const FVector& Vertex1 = StateData->Vertices[Index1];
        const FVector& Vertex2 = StateData->Vertices[Index2];

        FVector FaceNormal = -FVector::CrossProduct(Vertex1 - Vertex0, Vertex2 - Vertex0).GetSafeNormal();
        FVector FaceNormalWorld = StateData->ActorTransform.TransformVector(FaceNormal);

        float FaceDot = 0.0f;
        if (ViewInfo.ProjectionMode == ECameraProjectionMode::Perspective)
        {
          FVector Vertex0World = StateData->ActorTransform.TransformPosition(Vertex0);
          FaceDot = FVector::DotProduct(FaceNormalWorld, (ViewInfo.Location - Vertex0World).GetSafeNormal());
        }
        else if (ViewInfo.ProjectionMode == ECameraProjectionMode::Orthographic)
        {
          // get forward vector of viewinfo
          FVector Forward = ViewInfo.Rotation.Vector();
          FaceDot = FVector::DotProduct(FaceNormalWorld, -Forward);
        }

        if (FaceDot <= 0.0f)
        {
          continue;
        }

        // get the UVs of the face
        const FVector2D& Uv0 = StateData->Uvs[Index0];
        const FVector2D& Uv1 = StateData->Uvs[Index1];
        const FVector2D& Uv2 = StateData->Uvs[Index2];

        RasterizeTriangle(Uv0, Uv1, Uv2, StateData->TextureWidth, StateData->TextureHeight, [&](int X, int Y, const FVector& Barycentric)
          {
            int PixelIndex = X + Y * StateData->TextureWidth;
            if (PixelIndex < 0 || PixelIndex >= StateData->Pixels->Num())
            {
              return;
            }

            if
            (
              StateData->RenderData->bPreserveExisting &&
              (*StateData->Pixels)[PixelIndex].A >= StateData->RenderData->PreserveThreshold
            )
            {
              return;
            }

            // find the local position of the pixel
            FVector LocalPosition = Barycentric.X * Vertex0 + Barycentric.Y * Vertex1 + Barycentric.Z * Vertex2;
            FVector WorldPosition = StateData->ActorTransform.TransformPosition(LocalPosition);

            // project the world position to screen space
            FPlane Result = ViewProjectionMatrix.TransformFVector4(FVector4(WorldPosition, 1.f));
            if (Result.W <= 0.0f)
            {
              return;
            }

            // the result of this will be x and y coords in -1..1 projection space
            const float Rhw = 1.0f / Result.W;
            FPlane PosInScreenSpace = FPlane(Result.X * Rhw, Result.Y * Rhw, Result.Z * Rhw, Result.W);

            // Move from projection space to normalized 0..1 UI space
            FVector2D Uv
            (
              (PosInScreenSpace.X / 2.f) + 0.5f,
              1.f - (PosInScreenSpace.Y / 2.f) - 0.5f
            );

            if (Uv.X < 0.0f || Uv.X > 1.0f || Uv.Y < 0.0f || Uv.Y > 1.0f)
            {
              return;
            }

            const FComfyTexturesImageData& RawDepth = StateData->RenderData->RawDepth;

            // calculate the pixel coordinates
            int PixelX = FMath::FloorToInt(Uv.X * (RawDepth.Width - 1));
            int PixelY = FMath::FloorToInt(Uv.Y * (RawDepth.Height - 1));

            float ClosestDepth = RawDepth.Pixels[PixelX + PixelY * RawDepth.Width].R;

            if (ViewInfo.ProjectionMode == ECameraProjectionMode::Perspective)
            {
              FVector ViewSpacePoint = ViewMatrix.TransformPosition(WorldPosition);

              const float Eps = 5.0f;
              if (ViewSpacePoint.Z > ClosestDepth + Eps)
              {
                return;
              }
            }
            else
            {
              FVector4 ClipSpacePoint = ProjectionMatrix.TransformFVector4(FVector4(0.0f, 0.0f, ClosestDepth, 1.0f));
              float ClipSpaceDepth = ClipSpacePoint.Z / ClipSpacePoint.W;
              const float Eps = 0.01f;
              if (PosInScreenSpace.Z < ClipSpaceDepth - Eps)
              {
                return;
              }
            }

            // get the pixel color from the input texture
            FLinearColor Pixel = SampleBilinear(StateData->RenderData->OutputPixels, StateData->RenderData->OutputWidth,
              StateData->RenderData->OutputHeight, Uv);
            Pixel.A = FMath::Clamp(FMath::Abs(FaceDot), 0.0f, 1.0f);
            Pixel *= 255.0f;
            (*StateData->Pixels)[PixelIndex] = FColor(Pixel.R, Pixel.G, Pixel.B, Pixel.A);
          });
      }

      ExpandTextureIslands(*StateData->Pixels, StateData->TextureWidth, StateData->TextureHeight, 4);

      AsyncTask(ENamedThreads::GameThread, [StateData, Callback]()
        {
          UKismetSystemLibrary::TransactObject(StateData->Texture2D);

          FColor* MipData = (FColor*)StateData->Texture2D->Source.LockMip(0);
          if (MipData == nullptr)
          {
            UE_LOG(LogComfyTextures, Error, TEXT("Failed to lock mip 0 for texture %s."), *(StateData->Texture2D->GetName()));
            Callback(false);
            return false;
          }

          FMemory::Memcpy(MipData, StateData->Pixels->GetData(), StateData->Pixels->Num() * sizeof(FColor));

          StateData->Texture2D->Source.UnlockMip(0);
          StateData->Texture2D->MarkPackageDirty();
          StateData->Texture2D->UpdateResource();

          Callback(true);
          return true;
        });

      return true;
    });

  return true;
}

bool UComfyTexturesWidgetBase::ProcessRenderResults()
{
  if (!ValidateAllRequestsSucceeded())
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Not all requests succeeded"));
    TransitionToIdleState();
    return false;
  }

  if (RenderQueue.Num() == 0)
  {
    UE_LOG(LogComfyTextures, Error, TEXT("No requests to process"));
    TransitionToIdleState();
    return false;
  }

  LoadRenderResultImages([this](bool bSuccess)
    {
      if (!bSuccess)
      {
        UE_LOG(LogComfyTextures, Error, TEXT("Failed to load render result images"));
        TransitionToIdleState();
        return;
      }

      if (UKismetSystemLibrary::BeginTransaction("ComfyTextures", FText::FromString("Comfy Textures Process Actors"), nullptr) != 0)
      {
        UE_LOG(LogComfyTextures, Error, TEXT("Failed to begin transaction"));
        TransitionToIdleState();
        return;
      }

      TSharedPtr<int32> NumPending = MakeShared<int32>();

      for (AActor* Actor : ActorSet)
      {
        if (ProcessRenderResultForActor(Actor, [&, NumPending](bool bSuccess)
          {
            if (!bSuccess)
            {
              UE_LOG(LogComfyTextures, Warning, TEXT("Failed to process render result for actor %s"), *Actor->GetName());
            }

            (*NumPending)--;
            if (*NumPending == 0)
            {
              if (UKismetSystemLibrary::EndTransaction() != -1)
              {
                UE_LOG(LogComfyTextures, Warning, TEXT("Failed to end transaction"));
              }

              TransitionToIdleState();
            }

            return true;
          }))
        {
          (*NumPending)++;
        }
      }

      if (*NumPending == 0)
      {
        if (UKismetSystemLibrary::EndTransaction() != -1)
        {
          UE_LOG(LogComfyTextures, Warning, TEXT("Failed to end transaction"));
        }

        TransitionToIdleState();
      }
    });

  State = EComfyTexturesState::Processing;
  OnStateChanged(State);

  return true;
}

void UComfyTexturesWidgetBase::CancelJob()
{
  if (State != EComfyTexturesState::Rendering)
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Not rendering"));
    return;
  }

  InterruptRender();
  ClearRenderQueue();
  TransitionToIdleState();
}

static TArray<TSharedPtr<FJsonObject>> FindNodesByTitle(const FJsonObject& Workflow, const FString& Title)
{
  TArray<TSharedPtr<FJsonObject>> Nodes;

  for (const TPair<FString, TSharedPtr<FJsonValue>>& Node : Workflow.Values)
  {
    const TSharedPtr<FJsonObject>* NodeObject;
    if (!Node.Value->TryGetObject(NodeObject))
    {
      continue;
    }

    const TSharedPtr<FJsonObject>* Meta;
    if (!(*NodeObject)->TryGetObjectField("_meta", Meta))
    {
      continue;
    }
    if (!Meta->IsValid())
    {
      continue;
    }

    FString NodeTitle;
    if (!(*Meta)->TryGetStringField("title", NodeTitle))
    {
      continue;
    }

    if (NodeTitle == Title)
    {
      Nodes.Add(*NodeObject);
    }
  }

  return Nodes;
}

static void SetNodeInputProperty(FJsonObject& Workflow, const FString& NodeName, const FString& PropertyName, double Value)
{
  TArray<TSharedPtr<FJsonObject>> Nodes = FindNodesByTitle(Workflow, NodeName);

  for (TSharedPtr<FJsonObject>& Node : Nodes)
  {
    const TSharedPtr<FJsonObject>* Inputs;
    if (!Node->TryGetObjectField("inputs", Inputs) || !Inputs->IsValid())
    {
      continue;
    }

    if ((*Inputs)->HasField(PropertyName))
    {
      (*Inputs)->SetNumberField(PropertyName, Value);
    }
  }
}

static void SetNodeInputProperty(FJsonObject& Workflow, const FString& NodeName, const FString& PropertyName, int Value)
{
  TArray<TSharedPtr<FJsonObject>> Nodes = FindNodesByTitle(Workflow, NodeName);

  for (TSharedPtr<FJsonObject>& Node : Nodes)
  {
    const TSharedPtr<FJsonObject>* Inputs;
    if (!Node->TryGetObjectField("inputs", Inputs) || !Inputs->IsValid())
    {
      continue;
    }

    if ((*Inputs)->HasField(PropertyName))
    {
      (*Inputs)->SetNumberField(PropertyName, Value);
    }
  }
}

static void SetNodeInputProperty(FJsonObject& Workflow, const FString& NodeName, const FString& PropertyName, const FString& Value)
{
  TArray<TSharedPtr<FJsonObject>> Nodes = FindNodesByTitle(Workflow, NodeName);

  for (TSharedPtr<FJsonObject>& Node : Nodes)
  {
    const TSharedPtr<FJsonObject>* Inputs;
    if (!Node->TryGetObjectField("inputs", Inputs) || !Inputs->IsValid())
    {
      continue;
    }

    if ((*Inputs)->HasField(PropertyName))
    {
      (*Inputs)->SetStringField(PropertyName, Value);
    }
  }
}

static bool GetNodeInputProperty(FJsonObject& Workflow, const FString& NodeName, const FString& PropertyName, int& OutValue)
{
  TArray<TSharedPtr<FJsonObject>> Nodes = FindNodesByTitle(Workflow, NodeName);

  for (TSharedPtr<FJsonObject>& Node : Nodes)
  {
    const TSharedPtr<FJsonObject>* Inputs;
    if (!Node->TryGetObjectField("inputs", Inputs) || !Inputs->IsValid())
    {
      continue;
    }

    if (!(*Inputs)->TryGetNumberField(PropertyName, OutValue))
    {
      continue;
    }

    return true;
  }

  return false;
}

static bool GetNodeInputProperty(FJsonObject& Workflow, const FString& NodeName, const FString& PropertyName, FString& OutValue)
{
  TArray<TSharedPtr<FJsonObject>> Nodes = FindNodesByTitle(Workflow, NodeName);

  for (TSharedPtr<FJsonObject>& Node : Nodes)
  {
    const TSharedPtr<FJsonObject>* Inputs;
    if (!Node->TryGetObjectField("inputs", Inputs) || !Inputs->IsValid())
    {
      continue;
    }

    if (!(*Inputs)->TryGetStringField(PropertyName, OutValue))
    {
      continue;
    }

    return true;
  }

  return false;
}

static bool GetNodeInputProperty(FJsonObject& Workflow, const FString& NodeName, const FString& PropertyName, float& OutValue)
{
  TArray<TSharedPtr<FJsonObject>> Nodes = FindNodesByTitle(Workflow, NodeName);

  for (TSharedPtr<FJsonObject>& Node : Nodes)
  {
    const TSharedPtr<FJsonObject>* Inputs;
    if (!Node->TryGetObjectField("inputs", Inputs) || !Inputs->IsValid())
    {
      continue;
    }

    if (!(*Inputs)->TryGetNumberField(PropertyName, OutValue))
    {
      continue;
    }

    return true;
  }

  return false;
}

bool UComfyTexturesWidgetBase::QueueRender(const FComfyTexturesRenderOptions& RenderOpts, int& RequestIndex)
{
  if (!IsConnected())
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Not connected to ComfyUI"));
    return false;
  }

  FString WorkflowJsonPath = GetWorkflowJsonPath(RenderOpts.Mode);
  if (!FPaths::FileExists(WorkflowJsonPath))
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Workflow JSON file does not exist: %s"), *WorkflowJsonPath);
    return false;
  }

  FString JsonString = "";
  if (!FFileHelper::LoadFileToString(JsonString, *WorkflowJsonPath))
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Failed to load workflow JSON file: %s"), *WorkflowJsonPath);
    return false;
  }

  TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
  TSharedPtr<FJsonObject> Workflow;

  if (!FJsonSerializer::Deserialize(Reader, Workflow))
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Failed to deserialize workflow JSON"));
    return false;
  }

  SetNodeInputProperty(*Workflow, "positive_prompt", "text_g", RenderOpts.Params.PositivePrompt);
  SetNodeInputProperty(*Workflow, "positive_prompt", "text_l", RenderOpts.Params.PositivePrompt);
  SetNodeInputProperty(*Workflow, "positive_prompt", "text", RenderOpts.Params.PositivePrompt);

  SetNodeInputProperty(*Workflow, "negative_prompt", "text_g", RenderOpts.Params.NegativePrompt);
  SetNodeInputProperty(*Workflow, "negative_prompt", "text_l", RenderOpts.Params.NegativePrompt);
  SetNodeInputProperty(*Workflow, "negative_prompt", "text", RenderOpts.Params.NegativePrompt);

  int TotalSteps = RenderOpts.Params.Steps + RenderOpts.Params.RefinerSteps;

  int StartAtStep = 0;
  if (RenderOpts.Mode == EComfyTexturesMode::Refine)
  {
    // denoise = (steps - start_at_step) / steps
    StartAtStep = TotalSteps - RenderOpts.Params.DenoiseStrength * (float)TotalSteps;
    StartAtStep = FMath::Clamp(StartAtStep, 0, RenderOpts.Params.Steps);
  }

  SetNodeInputProperty(*Workflow, "sampler", "noise_seed", RenderOpts.Params.Seed);
  SetNodeInputProperty(*Workflow, "sampler", "cfg", RenderOpts.Params.Cfg);
  SetNodeInputProperty(*Workflow, "sampler", "steps", TotalSteps);
  SetNodeInputProperty(*Workflow, "sampler", "start_at_step", StartAtStep);
  SetNodeInputProperty(*Workflow, "sampler", "end_at_step", RenderOpts.Params.Steps);

  // SetNodeInputProperty(*Workflow, "sampler_refiner", "noise_seed", RenderOpts.Params.Seed);
  SetNodeInputProperty(*Workflow, "sampler_refiner", "cfg", RenderOpts.Params.Cfg);
  SetNodeInputProperty(*Workflow, "sampler_refiner", "steps", TotalSteps);
  SetNodeInputProperty(*Workflow, "sampler_refiner", "start_at_step", RenderOpts.Params.Steps);

  SetNodeInputProperty(*Workflow, "control_depth", "strength", RenderOpts.Params.ControlDepthStrength);
  SetNodeInputProperty(*Workflow, "control_canny", "strength", RenderOpts.Params.ControlCannyStrength);

  SetNodeInputProperty(*Workflow, "input_depth", "image", RenderOpts.DepthImageFilename);
  SetNodeInputProperty(*Workflow, "input_normals", "image", RenderOpts.NormalsImageFilename);
  SetNodeInputProperty(*Workflow, "input_color", "image", RenderOpts.ColorImageFilename);
  SetNodeInputProperty(*Workflow, "input_mask", "image", RenderOpts.MaskImageFilename);
  SetNodeInputProperty(*Workflow, "input_edge", "image", RenderOpts.EdgeMaskImageFilename);

  TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
  Payload->SetStringField("client_id", HttpClient->ClientId);
  Payload->SetObjectField("prompt", Workflow);

  RequestIndex = NextRequestIndex++;
  RenderQueue.Add(RequestIndex, MakeShared<FComfyTexturesRenderData>());

  TWeakObjectPtr<UComfyTexturesWidgetBase> WeakThis(this);

  return HttpClient->DoHttpPostRequest("prompt", Payload, [WeakThis, RequestIndex](const TSharedPtr<FJsonObject>& Response, bool bWasSuccessful)
    {
      if (!WeakThis.IsValid())
      {
        return;
      }

      UComfyTexturesWidgetBase* This = WeakThis.Get();

      if (!This->RenderQueue.Contains(RequestIndex))
      {
        UE_LOG(LogComfyTextures, Error, TEXT("Render queue does not contain request index"));
        return;
      }

      FComfyTexturesRenderData& Data = *This->RenderQueue[RequestIndex];

      if (!bWasSuccessful)
      {
        UE_LOG(LogComfyTextures, Error, TEXT("Failed to send render request"));
        Data.State = EComfyTexturesRenderState::Failed;
        This->HandleRenderStateChanged(Data);
        return;
      }

      FString PromptId;
      if (!Response->TryGetStringField("prompt_id", PromptId))
      {
        UE_LOG(LogComfyTextures, Error, TEXT("Failed to get prompt ID"));
        Data.State = EComfyTexturesRenderState::Failed;
        This->HandleRenderStateChanged(Data);
        return;
      }

      Data.PromptId = PromptId;
      Data.State = EComfyTexturesRenderState::Pending;

      if (Response->HasField("error"))
      {
        UE_LOG(LogComfyTextures, Error, TEXT("Render request failed"));
        Data.State = EComfyTexturesRenderState::Failed;
      }
      else
      {
        UE_LOG(LogComfyTextures, Verbose, TEXT("Render request successful"));
      }

      This->PromptIdToRequestIndex.Add(PromptId, RequestIndex);
      This->HandleRenderStateChanged(Data);
    });
}

void UComfyTexturesWidgetBase::InterruptRender() const
{
  if (!IsConnected())
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Not connected to ComfyUI"));
    return;
  }

  HttpClient->DoHttpPostRequest("interrupt", nullptr, [this](const TSharedPtr<FJsonObject>& Response, bool bWasSuccessful)
    {
      if (!bWasSuccessful)
      {
        UE_LOG(LogComfyTextures, Warning, TEXT("Failed to send interrupt request"));
        return;
      }

      UE_LOG(LogComfyTextures, Verbose, TEXT("Interrupt request successful"));
    });
}

void UComfyTexturesWidgetBase::ClearRenderQueue()
{
  if (!IsConnected())
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Not connected to ComfyUI"));
    return;
  }

  TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
  Payload->SetBoolField("clear", true);

  HttpClient->DoHttpPostRequest("queue", Payload, [this](const TSharedPtr<FJsonObject>& Response, bool bWasSuccessful)
    {
      if (!bWasSuccessful)
      {
        UE_LOG(LogComfyTextures, Warning, TEXT("Failed to send clear request"));
        return;
      }

      UE_LOG(LogComfyTextures, Verbose, TEXT("Clear request successful"));
    });
}

void UComfyTexturesWidgetBase::FreeComfyMemory(bool bUnloadModels)
{
  if (!IsConnected())
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Not connected to ComfyUI"));
    return;
  }

  TSharedPtr<FJsonObject> Payload = nullptr;

  if (bUnloadModels)
  {
    Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField("free_memory", true);
    Payload->SetBoolField("unload_models", true);

    HttpClient->DoHttpPostRequest("free", Payload, [this](const TSharedPtr<FJsonObject>& Response, bool bWasSuccessful)
      {
        if (!bWasSuccessful)
        {
          UE_LOG(LogComfyTextures, Warning, TEXT("Failed to send cleanup request"));
          return;
        }

        UE_LOG(LogComfyTextures, Verbose, TEXT("Cleanup request successful"));
      });
  }

  Payload = MakeShared<FJsonObject>();
  Payload->SetBoolField("clear", true);

  HttpClient->DoHttpPostRequest("history", Payload, [this](const TSharedPtr<FJsonObject>& Response, bool bWasSuccessful)
    {
      if (!bWasSuccessful)
      {
        UE_LOG(LogComfyTextures, Warning, TEXT("Failed to send history clear request"));
        return;
      }

      UE_LOG(LogComfyTextures, Verbose, TEXT("History clear request successful"));
    });
}

bool UComfyTexturesWidgetBase::PrepareActors(const TArray<AActor*>& Actors, const FComfyTexturesPrepareOptions& PrepareOpts)
{
  if (Actors.Num() == 0)
  {
    UE_LOG(LogComfyTextures, Error, TEXT("No actors to prepare"));
    return false;
  }

  if (PrepareOpts.BaseMaterial == nullptr)
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Base material is null"));
    return false;
  }

  // get pixels of reference texture
  UTexture2D* ReferenceTexture = PrepareOpts.ReferenceTexture;
  if (ReferenceTexture == nullptr)
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Reference texture is null"));
    return false;
  }

  void* ReferenceMipData = ReferenceTexture->Source.LockMip(0);
  if (ReferenceMipData == nullptr)
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Failed to lock texture mip data"));
    return false;
  }

  TArray<FColor> ReferencePixels;
  ReferencePixels.SetNumZeroed(ReferenceTexture->GetSizeX() * ReferenceTexture->GetSizeY());
  FMemory::Memcpy(ReferencePixels.GetData(), ReferenceMipData, ReferencePixels.Num() * sizeof(FColor));
  ReferenceTexture->Source.UnlockMip(0);

  UComfyTexturesSettings* Settings = GetMutableDefault<UComfyTexturesSettings>();

  FScopedTransaction Transaction(TEXT("Comfy Textures Prepare Actors"), LOCTEXT("ComfyTextures", "Prepare Actors"), nullptr);

  for (AActor* Actor : Actors)
  {
    // get the static mesh component
    UStaticMeshComponent* StaticMeshComponent = Actor->FindComponentByClass<UStaticMeshComponent>();

    // if the actor doesn't have a static mesh component, skip it
    if (StaticMeshComponent == nullptr)
    {
      continue;
    }

    // get the current material
    UMaterialInterface* Material = StaticMeshComponent->GetMaterial(0);

    // if the current material is a dynamic material instance which base material is the same as the prepare options base material, skip it
    if (Material != nullptr && Material->IsA<UMaterialInstanceDynamic>())
    {
      UMaterialInstanceDynamic* MaterialInstance = Cast<UMaterialInstanceDynamic>(Material);
      if (MaterialInstance->Parent == PrepareOpts.BaseMaterial)
      {
        continue;
      }
    }

    FString Id = FGuid::NewGuid().ToString();

    // create a new texture

    FBox2D ActorScreenBounds;
    if (!CalculateApproximateScreenBounds(Actor, PrepareOpts.ViewInfo, ActorScreenBounds))
    {
      UE_LOG(LogComfyTextures, Warning, TEXT("Failed to calculate screen bounds for actor %s."), *Actor->GetName());
      continue;
    }

    UE_LOG(LogComfyTextures, Verbose, TEXT("Actor %s screen bounds: %s."), *Actor->GetName(), *ActorScreenBounds.ToString());

    FVector2D SizeOnScreen = ActorScreenBounds.GetSize();
    float LargerSize = FMath::Max(SizeOnScreen.X, SizeOnScreen.Y);

    // get next power of two
    int TextureSize = FMath::Lerp((float)Settings->MinTextureSize, (float)Settings->MaxTextureSize, LargerSize);
    TextureSize = (float)TextureSize * Settings->TextureQualityMultiplier;

    // clamp to max texture size
    TextureSize = FMath::Clamp(TextureSize, Settings->MinTextureSize, Settings->MaxTextureSize);
    TextureSize = FMath::RoundUpToPowerOfTwo(TextureSize);

    UE_LOG(LogComfyTextures, Verbose, TEXT("Chosen texture size: %d for actor %s."), TextureSize, *Actor->GetName());

    FString TextureName = "GeneratedTexture_" + Id;

    // rescale reference texture to TextureSize
    TArray<FColor> RescaledReferencePixels;
    RescaledReferencePixels.SetNumZeroed(TextureSize * TextureSize);

    FImageUtils::ImageResize(ReferenceTexture->GetSizeX(), ReferenceTexture->GetSizeY(), ReferencePixels, TextureSize, TextureSize, RescaledReferencePixels, false);

    UTexture2D* Texture2D = CreateTexture2D(TextureSize, TextureSize, RescaledReferencePixels);
    if (Texture2D == nullptr)
    {
      UE_LOG(LogComfyTextures, Error, TEXT("Failed to create texture %s"), *TextureName);
      return false;
    }

    Texture2D->Rename(*TextureName);

    if (!CreateAssetPackage(Texture2D, "/Game/Textures/Generated/"))
    {
      UE_LOG(LogComfyTextures, Error, TEXT("Failed to create asset package for texture %s"), *TextureName);
      return false;
    }

    // create a new material instance
    UMaterialInstanceDynamic* MaterialInstance = UMaterialInstanceDynamic::Create(PrepareOpts.BaseMaterial, nullptr);
    MaterialInstance->SetTextureParameterValue(TEXT("BaseColor"), Texture2D);

    FString MaterialName = "GeneratedMaterial_" + Id;
    MaterialInstance->Rename(*MaterialName);

    if (!CreateAssetPackage(MaterialInstance, "/Game/Materials/Generated/"))
    {
      UE_LOG(LogComfyTextures, Error, TEXT("Failed to create asset package for material instance %s"), *MaterialName);
      return false;
    }

    StaticMeshComponent->Modify();

    // assign the material instance to the static mesh component
    StaticMeshComponent->SetMaterial(0, MaterialInstance);

    StaticMeshComponent->MarkPackageDirty();
  }

  return true;
}

bool UComfyTexturesWidgetBase::ParseWorkflowJson(const FString& JsonPath, FComfyTexturesWorkflowParams& OutParams)
{
  FString JsonString = "";
  if (!FFileHelper::LoadFileToString(JsonString, *JsonPath))
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Failed to load workflow JSON file: %s"), *JsonPath);
    return false;
  }

  TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
  TSharedPtr<FJsonObject> Workflow;

  if (!FJsonSerializer::Deserialize(Reader, Workflow))
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Failed to deserialize workflow JSON file: %s"), *JsonPath);
    return false;
  }

  GetNodeInputProperty(*Workflow, "positive_prompt", "text_g", OutParams.PositivePrompt);
  GetNodeInputProperty(*Workflow, "negative_prompt", "text_g", OutParams.NegativePrompt);

  GetNodeInputProperty(*Workflow, "sampler", "noise_seed", OutParams.Seed);
  GetNodeInputProperty(*Workflow, "sampler", "cfg", OutParams.Cfg);

  int TotalSteps = 0;
  GetNodeInputProperty(*Workflow, "sampler", "steps", TotalSteps);

  OutParams.RefinerSteps = 0;

  int RefinerStartAtStep = 0;
  if (GetNodeInputProperty(*Workflow, "sampler_refiner", "start_at_step", RefinerStartAtStep))
  {
    OutParams.RefinerSteps = TotalSteps - RefinerStartAtStep;
    OutParams.Steps = TotalSteps - OutParams.RefinerSteps;
  }
  else
  {
    OutParams.Steps = TotalSteps;
  }

  int StartAtStep = 0;
  GetNodeInputProperty(*Workflow, "sampler", "start_at_step", StartAtStep);

  // denoise = (steps - start_at_step) / steps
  OutParams.DenoiseStrength = (TotalSteps - StartAtStep) / (float)TotalSteps;

  GetNodeInputProperty(*Workflow, "sampler_refiner", "noise_seed", OutParams.Seed);
  GetNodeInputProperty(*Workflow, "sampler_refiner", "cfg", OutParams.Cfg);

  GetNodeInputProperty(*Workflow, "control_depth", "strength", OutParams.ControlDepthStrength);
  GetNodeInputProperty(*Workflow, "control_canny", "strength", OutParams.ControlCannyStrength);

  return true;
}

FString UComfyTexturesWidgetBase::GetWorkflowJsonPath(EComfyTexturesMode Mode) const
{
  FString PluginFolderPath = FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("ComfyTextures"));
  FString JsonPath = FPaths::Combine(PluginFolderPath, TEXT("/Content/Workflows/"));

  if (Mode == EComfyTexturesMode::Create)
  {
    JsonPath = FPaths::Combine(JsonPath, TEXT("ComfyTexturesDefaultWorkflow.json"));
  }
  else if (Mode == EComfyTexturesMode::Edit)
  {
    JsonPath = FPaths::Combine(JsonPath, TEXT("ComfyTexturesInpaintingWorkflow.json"));
  }
  else if (Mode == EComfyTexturesMode::Refine)
  {
    JsonPath = FPaths::Combine(JsonPath, TEXT("ComfyTexturesRefinementWorkflow.json"));
  }
  else
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Invalid mode"));
  }

  return JsonPath;
}

void UComfyTexturesWidgetBase::SetEditorFrameRate(int Fps)
{
  UComfyTexturesSettings* Settings = GetMutableDefault<UComfyTexturesSettings>();
  if (!Settings->bLimitEditorFps)
  {
    return;
  }

  GEditor->SetMaxFPS(Fps);
}

static void GetChildActorsRecursive(AActor* Actor, TSet<AActor*>& OutActors)
{
  if (Actor == nullptr)
  {
    return;
  }

  // get static mesh
  UStaticMeshComponent* StaticMeshComponent = Actor->FindComponentByClass<UStaticMeshComponent>();
  if (StaticMeshComponent != nullptr)
  {
    OutActors.Add(Actor);
  }

  for (AActor* ChildActor : Actor->Children)
  {
    GetChildActorsRecursive(ChildActor, OutActors);
  }
}

void UComfyTexturesWidgetBase::GetFlattenedSelectionSetWithChildren(TArray<AActor*>& OutActors) const
{
  OutActors.Empty();

  for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
  {
    OutActors.Add(Cast<AActor>(*It));
  }

  TArray<AActor*> AttachedActors;
  for (AActor* Actor : OutActors)
  {
    Actor->GetAttachedActors(AttachedActors, false, true);
  }

  TSet<AActor*> UniqueOutActors;
  UniqueOutActors.Append(OutActors);
  UniqueOutActors.Append(AttachedActors);

  OutActors.Empty();
  OutActors.Append(UniqueOutActors.Array());
}

bool UComfyTexturesWidgetBase::LoadParams()
{
  Params.Empty();

  // setup default params from workflow json

  for (int i = 0; i <= (int)EComfyTexturesMode::Refine; i++)
  {
    EComfyTexturesMode Mode = (EComfyTexturesMode)i;
    FString JsonPath = GetWorkflowJsonPath(Mode);

    FComfyTexturesWorkflowParams Param;
    Param.EditMaskMode = EComfyTexturesEditMaskMode::FromObject;

    if (!ParseWorkflowJson(JsonPath, Param))
    {
      UE_LOG(LogComfyTextures, Error, TEXT("Failed to parse workflow JSON file: %s"), *JsonPath);
    }

    Params.Add(Mode, Param);
  }

  FString PluginFolderPath = FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("ComfyTextures"));
  FString ConfigPath = FPaths::Combine(PluginFolderPath, TEXT("WidgetParams.json"));

  FString JsonString = "";

  if (!FFileHelper::LoadFileToString(JsonString, *ConfigPath))
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Failed to load widget params JSON file: %s"), *ConfigPath);
    return false;
  }

  TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

  TSharedPtr<FJsonObject> ParamsObject;
  if (!FJsonSerializer::Deserialize(Reader, ParamsObject))
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Failed to deserialize widget params JSON"));
    return false;
  }

  for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : ParamsObject->Values)
  {
    TSharedPtr<FJsonObject> ParamObject = Pair.Value->AsObject();

    FComfyTexturesWorkflowParams Param;
    ParamObject->TryGetStringField("positive_prompt", Param.PositivePrompt);
    ParamObject->TryGetStringField("negative_prompt", Param.NegativePrompt);
    ParamObject->TryGetNumberField("seed", Param.Seed);
    ParamObject->TryGetNumberField("cfg", Param.Cfg);
    ParamObject->TryGetNumberField("steps", Param.Steps);
    ParamObject->TryGetNumberField("refiner_steps", Param.RefinerSteps);
    ParamObject->TryGetNumberField("denoise_strength", Param.DenoiseStrength);
    ParamObject->TryGetNumberField("control_depth_strength", Param.ControlDepthStrength);
    ParamObject->TryGetNumberField("control_canny_strength", Param.ControlCannyStrength);

    float editMaskMode = 0.0f;
    if (ParamObject->TryGetNumberField("edit_mask_mode", editMaskMode))
    {
      Param.EditMaskMode = (EComfyTexturesEditMaskMode)editMaskMode;
    }

    if (Pair.Key == "create")
    {
      Params.Add(EComfyTexturesMode::Create, Param);
    }
    else if (Pair.Key == "edit")
    {
      Params.Add(EComfyTexturesMode::Edit, Param);
    }
    else if (Pair.Key == "refine")
    {
      Params.Add(EComfyTexturesMode::Refine, Param);
    }
    else
    {
      UE_LOG(LogComfyTextures, Error, TEXT("Invalid mode"));
    }
  }

  return true;
}

bool UComfyTexturesWidgetBase::SaveParams()
{
  FString PluginFolderPath = FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("ComfyTextures"));
  FString ConfigPath = FPaths::Combine(PluginFolderPath, TEXT("WidgetParams.json"));

  TSharedPtr<FJsonObject> ParamsObject = MakeShared<FJsonObject>();

  for (const TPair<EComfyTexturesMode, FComfyTexturesWorkflowParams>& Pair : Params)
  {
    TSharedPtr<FJsonObject> ParamObject = MakeShared<FJsonObject>();

    ParamObject->SetStringField("positive_prompt", Pair.Value.PositivePrompt);
    ParamObject->SetStringField("negative_prompt", Pair.Value.NegativePrompt);
    ParamObject->SetNumberField("seed", Pair.Value.Seed);
    ParamObject->SetNumberField("cfg", Pair.Value.Cfg);
    ParamObject->SetNumberField("steps", Pair.Value.Steps);
    ParamObject->SetNumberField("refiner_steps", Pair.Value.RefinerSteps);
    ParamObject->SetNumberField("denoise_strength", Pair.Value.DenoiseStrength);
    ParamObject->SetNumberField("control_depth_strength", Pair.Value.ControlDepthStrength);
    ParamObject->SetNumberField("control_canny_strength", Pair.Value.ControlCannyStrength);
    ParamObject->SetNumberField("edit_mask_mode", (float)Pair.Value.EditMaskMode);

    FString ModeString = "";
    if (Pair.Key == EComfyTexturesMode::Create)
    {
      ModeString = "create";
    }
    else if (Pair.Key == EComfyTexturesMode::Edit)
    {
      ModeString = "edit";
    }
    else if (Pair.Key == EComfyTexturesMode::Refine)
    {
      ModeString = "refine";
    }
    else
    {
      UE_LOG(LogComfyTextures, Error, TEXT("Invalid mode"));
      return false;
    }

    ParamsObject->SetObjectField(ModeString, ParamObject);
  }

  FString JsonString;
  TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
  FJsonSerializer::Serialize(ParamsObject.ToSharedRef(), Writer);

  return FFileHelper::SaveStringToFile(JsonString, *ConfigPath);
}

void UComfyTexturesWidgetBase::SetParams(EComfyTexturesMode Mode, const FComfyTexturesWorkflowParams& InParams)
{
  Params[Mode] = InParams;
}

void UComfyTexturesWidgetBase::GetParams(EComfyTexturesMode Mode, FComfyTexturesWorkflowParams& OutParams) const
{
  if (!Params.Contains(Mode))
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Params does not contain mode %d"), (int)Mode);
    return;
  }

  OutParams = Params[Mode];
}

void UComfyTexturesWidgetBase::HandleRenderStateChanged(const FComfyTexturesRenderData& Data)
{
  OnRenderStateChanged(Data.PromptId, Data);
}

FString UComfyTexturesWidgetBase::GetBaseUrl() const
{
  UComfyTexturesSettings* Settings = GetMutableDefault<UComfyTexturesSettings>();
  FString BaseUrl = Settings->ComfyUrl;

  if (!BaseUrl.StartsWith("http://") && !BaseUrl.StartsWith("https://"))
  {
    BaseUrl = "http://" + BaseUrl;
  }

  // strip trailing slash

  if (BaseUrl.EndsWith("/"))
  {
    BaseUrl = BaseUrl.LeftChop(1);
  }

  return BaseUrl;
}

void UComfyTexturesWidgetBase::HandleWebSocketMessage(const TSharedPtr<FJsonObject>& Message)
{
  FString MessageType;
  if (!Message->TryGetStringField("type", MessageType))
  {
    UE_LOG(LogComfyTextures, Warning, TEXT("Websocket message missing type field"));
    return;
  }

  const TSharedPtr<FJsonObject>* MessageData;
  if (!Message->TryGetObjectField("data", MessageData))
  {
    UE_LOG(LogComfyTextures, Warning, TEXT("Websocket message missing data field"));
    return;
  }

  FString PromptId;
  if (!(*MessageData)->TryGetStringField("prompt_id", PromptId))
  {
    UE_LOG(LogComfyTextures, Verbose, TEXT("Websocket message missing prompt_id field"));
    return;
  }

  if (!PromptIdToRequestIndex.Contains(PromptId))
  {
    UE_LOG(LogComfyTextures, Warning, TEXT("Received websocket message for unknown prompt_id: %s"), *PromptId);
    return;
  }

  int RequestIndex = PromptIdToRequestIndex[PromptId];
  if (!RenderQueue.Contains(RequestIndex))
  {
    UE_LOG(LogComfyTextures, Warning, TEXT("Received websocket message for unknown request index: %d"), RequestIndex);
    return;
  }

  FComfyTexturesRenderData& Data = *RenderQueue[RequestIndex];

  if (MessageType == "execution_start")
  {
    Data.State = EComfyTexturesRenderState::Started;
    Data.Progress = 0.0f;
    Data.CurrentNodeIndex = -1;
    HandleRenderStateChanged(Data);
  }
  else if (MessageType == "executing")
  {
    int CurrentNodeIndex;

    if (!(*MessageData)->TryGetNumberField("node", CurrentNodeIndex))
    {
      Data.State = EComfyTexturesRenderState::Finished;
      Data.Progress = 1.0f;
      Data.CurrentNodeIndex = -1;
      HandleRenderStateChanged(Data);
      return;
    }

    Data.CurrentNodeIndex = CurrentNodeIndex;
    HandleRenderStateChanged(Data);
  }
  else if (MessageType == "progress")
  {
    float Value;
    if (!(*MessageData)->TryGetNumberField("value", Value))
    {
      UE_LOG(LogComfyTextures, Warning, TEXT("Websocket message missing value field"));
      return;
    }

    float Max;
    if (!(*MessageData)->TryGetNumberField("max", Max))
    {
      UE_LOG(LogComfyTextures, Warning, TEXT("Websocket message missing max field"));
      return;
    }

    Data.Progress = Value / Max;
    HandleRenderStateChanged(Data);
  }
  else if (MessageType == "executed")
  {
    const TSharedPtr<FJsonObject>* OutputData;
    if (!(*MessageData)->TryGetObjectField("output", OutputData))
    {
      UE_LOG(LogComfyTextures, Warning, TEXT("Websocket message missing output field"));
      return;
    }

    // "output": {"images": [{"filename": "ComfyUI_00062_.png", "subfolder": "", "type": "output"}]}, "prompt_id": "30840158-3b72-4c3e-9112-7efea0a3a2a8"}
    const TArray<TSharedPtr<FJsonValue>>* Images;

    if (!(*OutputData)->TryGetArrayField("images", Images))
    {
      UE_LOG(LogComfyTextures, Warning, TEXT("Websocket message missing images field"));
      return;
    }

    for (const TSharedPtr<FJsonValue>& Image : *Images)
    {
      const TSharedPtr<FJsonObject>* ImageObject;
      if (!Image->TryGetObject(ImageObject))
      {
        UE_LOG(LogComfyTextures, Warning, TEXT("Websocket message missing image object"));
        return;
      }

      FString Filename;
      if (!(*ImageObject)->TryGetStringField("filename", Filename))
      {
        UE_LOG(LogComfyTextures, Warning, TEXT("Websocket message missing filename field"));
        return;
      }

      FString Subfolder;
      if (!(*ImageObject)->TryGetStringField("subfolder", Subfolder))
      {
        UE_LOG(LogComfyTextures, Warning, TEXT("Websocket message missing subfolder field"));
        return;
      }

      FString Type;
      if (!(*ImageObject)->TryGetStringField("type", Type))
      {
        UE_LOG(LogComfyTextures, Warning, TEXT("Websocket message missing type field"));
        return;
      }

      if (Type != "output")
      {
        continue;
      }

      Data.OutputFileNames.Add(Filename);
    }

    HandleRenderStateChanged(Data);
  }
  else
  {
    UE_LOG(LogComfyTextures, Verbose, TEXT("Unknown websocket message type: %s"), *MessageType);
  }
}

bool UComfyTexturesWidgetBase::ReadRenderTargetPixels(UTextureRenderTarget2D* InputTexture, EComfyTexturesRenderTextureMode Mode, FComfyTexturesImageData& OutImage) const
{
  if (InputTexture == nullptr)
  {
    return false;
  }

  FTextureRenderTargetResource* RenderTargetResource = InputTexture->GameThread_GetRenderTargetResource();
  if (RenderTargetResource == nullptr)
  {
    return false;
  }

  static TArray<FFloat16Color> Pixels;
  Pixels.SetNumUninitialized(InputTexture->SizeX * InputTexture->SizeY);
  if (!RenderTargetResource->ReadFloat16Pixels(Pixels))
  {
    return false;
  }

  OutImage.Width = InputTexture->SizeX;
  OutImage.Height = InputTexture->SizeY;
  OutImage.Pixels.SetNum(Pixels.Num());

  if (Mode == EComfyTexturesRenderTextureMode::Depth)
  {
    float MinDepth = FLT_MAX;
    float MaxDepth = -FLT_MAX;

    for (int Index = 0; Index < Pixels.Num(); Index++)
    {
      float Depth = Pixels[Index].A;
      if (Depth >= 65504.0f)
      {
        continue;
      }

      MinDepth = FMath::Min(MinDepth, Depth);
      MaxDepth = FMath::Max(MaxDepth, Depth);
    }

    for (int Index = 0; Index < Pixels.Num(); Index++)
    {
      float Depth = Pixels[Index].A;
      Depth = FMath::Clamp(Depth, MinDepth, MaxDepth);
      Depth = (Depth - MinDepth) / (MaxDepth - MinDepth);
      Depth = FMath::Clamp(Depth, 0.0f, 1.0f);
      Depth = 1.0f - Depth;

      OutImage.Pixels[Index].R = Depth;
      OutImage.Pixels[Index].G = Depth;
      OutImage.Pixels[Index].B = Depth;
      OutImage.Pixels[Index].A = 1.0f;
    }
  }
  else if (Mode == EComfyTexturesRenderTextureMode::RawDepth)
  {
    for (int Index = 0; Index < Pixels.Num(); Index++)
    {
      float Depth = Pixels[Index].A;
      OutImage.Pixels[Index].R = Depth;
      OutImage.Pixels[Index].G = Depth;
      OutImage.Pixels[Index].B = Depth;
      OutImage.Pixels[Index].A = 1.0f;
    }
  }
  else if (Mode == EComfyTexturesRenderTextureMode::Normals)
  {
    for (int Index = 0; Index < Pixels.Num(); Index++)
    {
      FVector Normal = FVector(Pixels[Index].R, Pixels[Index].G, Pixels[Index].B);
      Normal = Normal.GetSafeNormal();
      Normal = (Normal + 1.0f) / 2.0f;
      OutImage.Pixels[Index].R = Normal.X;
      OutImage.Pixels[Index].G = Normal.Y;
      OutImage.Pixels[Index].B = Normal.Z;
      OutImage.Pixels[Index].A = 1.0f;
    }
  }
  else if (Mode == EComfyTexturesRenderTextureMode::Color)
  {
    for (int Index = 0; Index < Pixels.Num(); Index++)
    {
      OutImage.Pixels[Index].R = Pixels[Index].R;
      OutImage.Pixels[Index].G = Pixels[Index].G;
      OutImage.Pixels[Index].B = Pixels[Index].B;
      OutImage.Pixels[Index].A = 1.0f;
    }
  }
  else
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Unknown render texture mode: %d"), (int)Mode);
    return false;
  }

  return true;
}

bool UComfyTexturesWidgetBase::ConvertImageToPng(const FComfyTexturesImageData& Image, TArray64<uint8>& OutBytes) const
{
  UE_LOG(LogComfyTextures, Verbose, TEXT("Converting image to PNG with Width: %d, Height: %d"), Image.Width, Image.Height);

  TArray<FColor> Image8;
  Image8.SetNum(Image.Pixels.Num());
  for (int Index = 0; Index < Image.Pixels.Num(); Index++)
  {
    FLinearColor Pixel = Image.Pixels[Index];
    // convert from linear to sRGB
    Pixel.R = FMath::Pow(Pixel.R, 1.0f / 2.2f);
    Pixel.G = FMath::Pow(Pixel.G, 1.0f / 2.2f);
    Pixel.B = FMath::Pow(Pixel.B, 1.0f / 2.2f);

    FColor OutPixel;
    OutPixel.R = FMath::Clamp(Pixel.R, 0.0f, 1.0f) * 255.0f;
    OutPixel.G = FMath::Clamp(Pixel.G, 0.0f, 1.0f) * 255.0f;
    OutPixel.B = FMath::Clamp(Pixel.B, 0.0f, 1.0f) * 255.0f;
    OutPixel.A = FMath::Clamp(Pixel.A, 0.0f, 1.0f) * 255.0f;
    Image8[Index] = OutPixel;
  }

  FImageUtils::PNGCompressImageArray(Image.Width, Image.Height, Image8, OutBytes);
  return true;
}

bool UComfyTexturesWidgetBase::UploadImages(const TArray<FComfyTexturesImageData>& Images, const TArray<FString>& FileNames, TFunction<void(const TArray<FString>&, bool)> Callback) const
{
  if (Images.Num() != FileNames.Num())
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Image and filename count do not match"));
    Callback(TArray<FString>(), false);
    return false;
  }

  // Shared state for tracking task completion and results
  struct SharedState
  {
    int32 RemainingTasks;
    TArray<FString> ResultFileNames;
    FThreadSafeBool bAllSuccessful = true;
  };
  TSharedPtr<SharedState> StateData = MakeShared<SharedState>();
  StateData->RemainingTasks = Images.Num();
  StateData->ResultFileNames.AddDefaulted(FileNames.Num());

  for (int32 Index = 0; Index < Images.Num(); ++Index)
  {
    Async(EAsyncExecution::ThreadPool, [this, Image = Images[Index], FileName = FileNames[Index], StateData, Index, Callback]()
      {
        TArray64<uint8> PngData;
        if (!ConvertImageToPng(Image, PngData))
        {
          StateData->bAllSuccessful = false;
          if (--StateData->RemainingTasks == 0)
          {
            Callback(StateData->ResultFileNames, false);
          }
          return;
        }

        HttpClient->DoHttpFileUpload("upload/image", PngData, FileName, [StateData, Index, Callback](const TSharedPtr<FJsonObject>& Response, bool bWasSuccessful)
          {
            if (!bWasSuccessful)
            {
              UE_LOG(LogComfyTextures, Error, TEXT("Failed to upload image"));
              StateData->bAllSuccessful = false;
            }
            else
            {
              FString ResultFileName;
              if (Response->TryGetStringField("name", ResultFileName))
              {
                StateData->ResultFileNames[Index] = ResultFileName;
              }
              else
              {
                UE_LOG(LogComfyTextures, Error, TEXT("Failed to get image url"));
                StateData->bAllSuccessful = false;
              }
            }

            // Check if this is the last task
            if (--StateData->RemainingTasks == 0)
            {
              Callback(StateData->ResultFileNames, StateData->bAllSuccessful);
            }
          });
      });
  }

  return true;
}

bool UComfyTexturesWidgetBase::DownloadImage(const FString& FileName, TFunction<void(TArray<FColor>, int, int, bool)> Callback) const
{
  FString Url = "view?filename=" + FileName;

  return HttpClient->DoHttpGetRequestRaw(Url, [Callback](const TArray<uint8>& PngData, bool bWasSuccessful)
    {
      TArray<FColor> Pixels;

      if (!bWasSuccessful)
      {
        UE_LOG(LogComfyTextures, Error, TEXT("Failed to download image"));
        Callback(Pixels, 0, 0, false);
        return;
      }

      // Create an image wrapper using the PNG format
      IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
      TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

      // Set the compressed data for the image wrapper
      if (!ImageWrapper.IsValid() || !ImageWrapper->SetCompressed(PngData.GetData(), PngData.Num()))
      {
        UE_LOG(LogComfyTextures, Error, TEXT("Failed to decompress image"));
        Callback(Pixels, 0, 0, false);
        return;
      }

      // Decompress the image data
      TArray<uint8> RawData;
      if (!ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, RawData))
      {
        UE_LOG(LogComfyTextures, Error, TEXT("Failed to decompress image"));
        Callback(Pixels, 0, 0, false);
        return;
      }

      int Width = ImageWrapper->GetWidth();
      int Height = ImageWrapper->GetHeight();

      Pixels.SetNumUninitialized(Width * Height);

      // Copy the decompressed pixel data

      const uint8* PixelData = RawData.GetData();

      for (int32 PixelIndex = 0; PixelIndex < Pixels.Num(); ++PixelIndex)
      {
        int32 Index = PixelIndex * 4; // 4 bytes per pixel (BGRA)
        FColor& Pixel = Pixels[PixelIndex];
        Pixel.B = PixelData[Index];
        Pixel.G = PixelData[Index + 1];
        Pixel.R = PixelData[Index + 2];
        Pixel.A = PixelData[Index + 3];
      }

      Callback(Pixels, Width, Height, true);
    });
}

// calculate the approximate screen bounds of an actor using the actor bounds and the camera view info
bool UComfyTexturesWidgetBase::CalculateApproximateScreenBounds(AActor* Actor, const FMinimalViewInfo& ViewInfo, FBox2D& OutBounds) const
{
  if (Actor == nullptr)
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Actor is null."));
    return false;
  }

  FBox ActorBounds = Actor->GetComponentsBoundingBox(true);
  FVector ActorExtent = ActorBounds.GetExtent();

  FVector Corners[8];
  Corners[0] = FVector(ActorExtent.X, ActorExtent.Y, ActorExtent.Z);
  Corners[1] = FVector(ActorExtent.X, ActorExtent.Y, -ActorExtent.Z);
  Corners[2] = FVector(ActorExtent.X, -ActorExtent.Y, ActorExtent.Z);
  Corners[3] = FVector(ActorExtent.X, -ActorExtent.Y, -ActorExtent.Z);
  Corners[4] = FVector(-ActorExtent.X, ActorExtent.Y, ActorExtent.Z);
  Corners[5] = FVector(-ActorExtent.X, ActorExtent.Y, -ActorExtent.Z);
  Corners[6] = FVector(-ActorExtent.X, -ActorExtent.Y, ActorExtent.Z);
  Corners[7] = FVector(-ActorExtent.X, -ActorExtent.Y, -ActorExtent.Z);

  FVector ActorCenter = ActorBounds.GetCenter();
  for (int CornerIndex = 0; CornerIndex < 8; CornerIndex++)
  {
    Corners[CornerIndex] = ActorCenter + Corners[CornerIndex];
  }

  TOptional<FMatrix> CustomProjectionMatrix;
  FMatrix ViewMatrix;
  FMatrix ProjectionMatrix;
  FMatrix ViewProjectionMatrix;
  UGameplayStatics::CalculateViewProjectionMatricesFromMinimalView(ViewInfo, CustomProjectionMatrix,
    ViewMatrix, ProjectionMatrix, ViewProjectionMatrix);

  // project the corners onto the screen
  FVector2D ScreenCorners[8];
  for (int CornerIndex = 0; CornerIndex < 8; CornerIndex++)
  {
    FVector4 HomogeneousPosition = ViewProjectionMatrix.TransformFVector4(FVector4(Corners[CornerIndex], 1.0f));
    ScreenCorners[CornerIndex] = FVector2D(HomogeneousPosition.X / HomogeneousPosition.W, HomogeneousPosition.Y / HomogeneousPosition.W);
  }

  // find the min and max screen coordinates
  float MinX = FLT_MAX;
  float MinY = FLT_MAX;
  float MaxX = -FLT_MAX;
  float MaxY = -FLT_MAX;

  for (int CornerIndex = 0; CornerIndex < 8; CornerIndex++)
  {
    MinX = FMath::Min(MinX, ScreenCorners[CornerIndex].X);
    MinY = FMath::Min(MinY, ScreenCorners[CornerIndex].Y);
    MaxX = FMath::Max(MaxX, ScreenCorners[CornerIndex].X);
    MaxY = FMath::Max(MaxY, ScreenCorners[CornerIndex].Y);
  }

  // clamp the screen coordinates to the screen bounds
  MinX = FMath::Clamp(MinX, -1.0f, 1.0f);
  MinY = FMath::Clamp(MinY, -1.0f, 1.0f);
  MaxX = FMath::Clamp(MaxX, -1.0f, 1.0f);
  MaxY = FMath::Clamp(MaxY, -1.0f, 1.0f);

  FVector2D Min = FVector2D(MinX, MinY);
  FVector2D Max = FVector2D(MaxX, MaxY);

  // convert from [-1, 1] to [0, 1]
  Min = Min * 0.5f + FVector2D(0.5f, 0.5f);
  Max = Max * 0.5f + FVector2D(0.5f, 0.5f);

  OutBounds = FBox2D(Min, Max);
  return true;
}

UTexture2D* UComfyTexturesWidgetBase::CreateTexture2D(int Width, int Height, const TArray<FColor>& Pixels) const
{
  if (Pixels.Num() != Width * Height)
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Pixels.Num() != Width * Height"));
    return nullptr;
  }

  UTexture2D* Texture2D = NewObject<UTexture2D>(UTexture2D::StaticClass());
  Texture2D->SetFlags(RF_Transactional);
  Texture2D->SRGB = true;
  Texture2D->bPreserveBorder = true;
  Texture2D->Filter = TF_Trilinear;
  Texture2D->AddToRoot();

  FTexturePlatformData* PlatformData = new FTexturePlatformData();
  PlatformData->SizeX = Width;
  PlatformData->SizeY = Height;
  PlatformData->PixelFormat = PF_B8G8R8A8;

  FTexture2DMipMap* Mip = new FTexture2DMipMap();
  Mip->SizeX = Width;
  Mip->SizeY = Height;
  Mip->BulkData.Lock(LOCK_READ_WRITE);
  int NumBytes = Width * Height * 4;
  Mip->BulkData.Realloc(NumBytes);
  Mip->BulkData.Unlock();

  void* TextureData = Mip->BulkData.Lock(LOCK_READ_WRITE);
  FMemory::Memcpy(TextureData, Pixels.GetData(), NumBytes);
  Mip->BulkData.Unlock();

  PlatformData->Mips.Add(Mip);

  Texture2D->SetPlatformData(PlatformData);
  Texture2D->UpdateResource();

  Texture2D->Source.Init(Width, Height, 1, PlatformData->Mips.Num(), ETextureSourceFormat::TSF_BGRA8, nullptr);

  // copy all the mip data

  for (int MipIndex = 0; MipIndex < PlatformData->Mips.Num(); MipIndex++)
  {
    Mip = &PlatformData->Mips[MipIndex];

    void* MipData = Texture2D->Source.LockMip(MipIndex);
    FMemory::Memcpy(MipData, Mip->BulkData.Lock(LOCK_READ_ONLY), Mip->BulkData.GetBulkDataSize());
    Mip->BulkData.Unlock();
    Texture2D->Source.UnlockMip(MipIndex);
  }

  return Texture2D;
}

bool UComfyTexturesWidgetBase::CreateAssetPackage(UObject* Asset, FString PackagePath) const
{
  if (Asset == nullptr)
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Asset is null."));
    return false;
  }

  if (PackagePath.Len() == 0)
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Package path is empty."));
    return false;
  }

  Asset->AddToRoot();
  Asset->SetFlags(RF_Public | RF_Standalone | RF_MarkAsRootSet);
  FAssetRegistryModule::AssetCreated(Asset);

  FString PackageName = PackagePath;
  if (PackageName[PackageName.Len() - 1] != '/')
  {
    PackageName += "/";
  }

  PackageName += Asset->GetName();

  UPackage* Package = CreatePackage(*PackageName);
  Package->FullyLoad();

  Asset->Rename(nullptr, Package);
  Package->MarkPackageDirty();

  FSavePackageArgs SaveArgs;
  SaveArgs.SaveFlags = SAVE_None;
  SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

  SaveArgs.Error = (FOutputDevice*)GLogConsole;

  FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());

  UE_LOG(LogComfyTextures, Verbose, TEXT("Saving asset %s to %s"), *Asset->GetName(), *PackageFileName);

  if (!UPackage::SavePackage(Package, Asset, *PackageFileName, SaveArgs))
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Failed to save package to %s"), *PackageFileName);
    return false;
  }

  return true;
}

void UComfyTexturesWidgetBase::CreateEditMaskFromImage(const TArray<FLinearColor>& Pixels, TArray<FLinearColor>& OutPixels) const
{
  // every pixel that's approximately 1, 0, 1 is considered a mask pixel
  // every other pixel is considered a non-mask pixel and is set to 0, 0, 0, 0

  OutPixels.SetNum(Pixels.Num());

  for (int Index = 0; Index < Pixels.Num(); Index++)
  {
    FLinearColor Pixel = Pixels[Index];
    float Epsilon = 0.05f;

    if (FMath::Abs(Pixel.R - 1.0f) < Epsilon && Pixel.G < Epsilon && FMath::Abs(Pixel.B - 1.0f) < Epsilon)
    {
      OutPixels[Index] = FLinearColor(1.0f, 1.0f, 1.0f, 1.0f);
    }
    else
    {
      OutPixels[Index] = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
    }
  }
}

// Sobel operator kernels for x and y directions
static const int SobelX[3][3] = { {-1, 0, 1}, {-2, 0, 2}, {-1, 0, 1} };
static const int SobelY[3][3] = { {-1, -2, -1}, {0, 0, 0}, {1, 2, 1} };

static float ComputeDepthGradient(const FComfyTexturesImageData& Image, int X, int Y)
{
  float GradX = 0.0f;
  float GradY = 0.0f;

  // Apply the Sobel operator
  for (int I = -1; I <= 1; I++)
  {
    for (int J = -1; J <= 1; J++)
    {
      int PixelX = FMath::Clamp(X + I, 0, Image.Width - 1);
      int PixelY = FMath::Clamp(Y + J, 0, Image.Height - 1);

      float Value = Image.Pixels[PixelY * Image.Width + PixelX].R; // Assuming depth is stored in the red channel

      GradX += Value * SobelX[I + 1][J + 1];
      GradY += Value * SobelY[I + 1][J + 1];
    }
  }

  return FMath::Sqrt(GradX * GradX + GradY * GradY);
}

static float ComputeNormalsGradient(const FComfyTexturesImageData& Image, int X, int Y)
{
  FVector GradX(0.0f, 0.0f, 0.0f);
  FVector GradY(0.0f, 0.0f, 0.0f);

  // Apply the Sobel operator
  for (int I = -1; I <= 1; I++)
  {
    for (int J = -1; J <= 1; J++)
    {
      int PixelX = FMath::Clamp(X + I, 0, Image.Width - 1);
      int PixelY = FMath::Clamp(Y + J, 0, Image.Height - 1);

      FVector Normal = FVector
      (
        Image.Pixels[PixelY * Image.Width + PixelX].R,
        Image.Pixels[PixelY * Image.Width + PixelX].G,
        Image.Pixels[PixelY * Image.Width + PixelX].B
      );

      Normal -= FVector(0.5f, 0.5f, 0.5f);
      Normal *= 2.0f;
      Normal = Normal.GetSafeNormal();

      GradX += Normal * SobelX[I + 1][J + 1];
      GradY += Normal * SobelY[I + 1][J + 1];
    }
  }

  // Calculate the gradient magnitude
  FVector Gradient = GradX + GradY;
  return Gradient.Size();
}

static float ComputeImageGradient(const FComfyTexturesImageData& Image, bool bIsDepth, TArray<float>& OutGrad)
{
  OutGrad.SetNumUninitialized(Image.Pixels.Num());

  float MaxGradient = -FLT_MAX;

  for (int Y = 0; Y < Image.Height; Y++)
  {
    for (int X = 0; X < Image.Width; X++)
    {
      float Gradient = bIsDepth ? ComputeDepthGradient(Image, X, Y) : ComputeNormalsGradient(Image, X, Y);
      OutGrad[Y * Image.Width + X] = Gradient;

      MaxGradient = FMath::Max(MaxGradient, Gradient);
    }
  }

  if (FMath::IsNearlyZero(MaxGradient))
  {
    return 0.0f;
  }

  float AverageMagnitude = 0.0f;

  for (int Y = 0; Y < Image.Height; Y++)
  {
    for (int X = 0; X < Image.Width; X++)
    {
      OutGrad[Y * Image.Width + X] /= MaxGradient;
      AverageMagnitude += OutGrad[Y * Image.Width + X];
    }
  }

  AverageMagnitude /= Image.Pixels.Num();
  return AverageMagnitude;
}

static float ComputeAdaptiveThreshold(const TArray<float>& Grad, float AverageGradient, float BaseThreshold, float ScaleFactor = 1.0f)
{
  return BaseThreshold + ScaleFactor * AverageGradient;
}

void UComfyTexturesWidgetBase::CreateEdgeMask(const FComfyTexturesImageData& Depth, const FComfyTexturesImageData& Normals, FComfyTexturesImageData& OutEdgeMask) const
{
  if (Depth.Width != Normals.Width || Depth.Height != Normals.Height)
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Depth and normals images have different dimensions."));
    return;
  }

  // perform edge detection on the depth and normals images
  OutEdgeMask.Width = Depth.Width;
  OutEdgeMask.Height = Depth.Height;
  OutEdgeMask.Pixels.SetNumUninitialized(Depth.Pixels.Num());

  TArray<float> DepthGrad;
  float AvgDepth = ComputeImageGradient(Depth, true, DepthGrad);

  TArray<float> NormalsGrad;
  float AvgNormals = ComputeImageGradient(Normals, false, NormalsGrad);

  const float DepthBaseThreshold = 0.01f;
  const float NormalsBaseThreshold = 0.1f;
  const float DepthScale = 8.0f;
  const float NormalsScale = 0.8f;

  float DepthThreshold = ComputeAdaptiveThreshold(DepthGrad, AvgDepth, DepthBaseThreshold);
  float NormalsThreshold = ComputeAdaptiveThreshold(NormalsGrad, AvgNormals, NormalsBaseThreshold);

  // Assuming Depth and Normals are of the same dimensions
  for (int Y = 0; Y < Depth.Height; Y++)
  {
    for (int X = 0; X < Depth.Width; X++)
    {
      // Compute gradients
      float DepthGradient = DepthGrad[Y * Depth.Width + X];
      float NormalsGradient = NormalsGrad[Y * Depth.Width + X];

      // Apply thresholds
      DepthGradient = (DepthGradient >= DepthThreshold) ? DepthGradient : 0.0f;
      NormalsGradient = (NormalsGradient >= NormalsThreshold) ? NormalsGradient : 0.0f;

      // Combine gradients for edge strength
      float EdgeStrength = FMath::Max(DepthGradient * DepthScale, NormalsGradient * NormalsScale);
      EdgeStrength = FMath::Clamp(EdgeStrength, 0.0f, 1.0f);

      // Set the pixel value in the output mask
      OutEdgeMask.Pixels[Y * Depth.Width + X] = FLinearColor(EdgeStrength, EdgeStrength, EdgeStrength, 1.0f);
    }
  }
}

void UComfyTexturesWidgetBase::LoadRenderResultImages(TFunction<void(bool)> Callback)
{
  // Shared state for tracking task completion and results
  struct SharedState
  {
    int32 RemainingTasks;
    FThreadSafeBool bAllSuccessful = true;
  };
  TSharedPtr<SharedState> StateData = MakeShared<SharedState>();
  StateData->RemainingTasks = RenderQueue.Num();

  for (TPair<int, FComfyTexturesRenderDataPtr>& Pair : RenderQueue)
  {
    int Index = Pair.Key;
    FComfyTexturesRenderDataPtr RenderData = Pair.Value;

    Async(EAsyncExecution::ThreadPool, [this, StateData, RenderData, Callback]()
      {
        FString FileName = RenderData->OutputFileNames[0];
        bool bSuccess = DownloadImage(FileName, [this, FileName, StateData, RenderData, Callback](TArray<FColor> Pixels, int Width, int Height, bool bWasSuccessful)
          {
            if (!bWasSuccessful)
            {
              UE_LOG(LogComfyTextures, Error, TEXT("Failed to download image %s"), *FileName);
              StateData->bAllSuccessful = false;
            }
            else
            {
              RenderData->OutputPixels = Pixels;
              RenderData->OutputWidth = Width;
              RenderData->OutputHeight = Height;
            }

            // Check if this is the last task
            if (--StateData->RemainingTasks == 0)
            {
              Callback(StateData->bAllSuccessful);
            }
          });

        if (!bSuccess)
        {
          UE_LOG(LogComfyTextures, Error, TEXT("Failed to download image %s"), *FileName);
          StateData->bAllSuccessful = false;

          // Check if this is the last task
          if (--StateData->RemainingTasks == 0)
          {
            Callback(StateData->bAllSuccessful);
          }
        }
      });
  }
}

void UComfyTexturesWidgetBase::TransitionToIdleState()
{
  RenderQueue.Empty();
  PromptIdToRequestIndex.Empty();
  ActorSet.Empty();

  State = EComfyTexturesState::Idle;
  OnStateChanged(State);
}

bool UComfyTexturesWidgetBase::CreateCameraTransforms(AActor* Actor, const FComfyTexturesRenderOptions& RenderOpts, TArray<FMinimalViewInfo>& OutViewInfos) const
{
  if (Actor == nullptr)
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Actor is null."));
    return false;
  }

  OutViewInfos.Empty();

  if (RenderOpts.CameraMode == EComfyTexturesCameraMode::EditorCamera)
  {
    // get current editor viewport camera transform
    FEditorViewportClient* EditorViewportClient = (FEditorViewportClient*)GEditor->GetActiveViewport()->GetClient();

    // get the camera transform
    const FViewportCameraTransform& CameraTransform = EditorViewportClient->GetViewTransform();

    // create a minimal view info from the camera transform
    FMinimalViewInfo ViewInfo;
    ViewInfo.Location = CameraTransform.GetLocation();
    ViewInfo.Rotation = CameraTransform.GetRotation();
    ViewInfo.FOV = EditorViewportClient->ViewFOV;
    ViewInfo.OrthoWidth = EditorViewportClient->GetOrthoUnitsPerPixel(EditorViewportClient->Viewport) * EditorViewportClient->Viewport->GetSizeXY().X;
    ViewInfo.ProjectionMode = EditorViewportClient->IsOrtho() ? ECameraProjectionMode::Orthographic : ECameraProjectionMode::Perspective;
    ViewInfo.AspectRatio = 1.0f; // EditorViewportClient->AspectRatio;
    ViewInfo.OrthoNearClipPlane = EditorViewportClient->GetNearClipPlane();
    ViewInfo.PerspectiveNearClipPlane = EditorViewportClient->GetNearClipPlane();

    OutViewInfos.Add(ViewInfo);
  }
  else if (RenderOpts.CameraMode == EComfyTexturesCameraMode::ExistingCamera)
  {
    if (RenderOpts.ExistingCamera == nullptr)
    {
      UE_LOG(LogComfyTextures, Error, TEXT("Existing camera is null."));
      return false;
    }

    // get camera component from camera actor

    UCameraComponent* CameraComponent = RenderOpts.ExistingCamera->FindComponentByClass<UCameraComponent>();
    if (CameraComponent == nullptr)
    {
      UE_LOG(LogComfyTextures, Error, TEXT("Camera component not found."));
      return false;
    }

    FMinimalViewInfo ViewInfo;
    CameraComponent->GetCameraView(0.0f, ViewInfo);

    if (!FMath::IsNearlyEqual(ViewInfo.AspectRatio, 1.0f))
    {
      UE_LOG(LogComfyTextures, Warning, TEXT("Camera aspect ratio is not 1.0, overriding it."));
      ViewInfo.AspectRatio = 1.0f;
    }

    OutViewInfos.Add(ViewInfo);
  }
  else
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Unsupported camera mode."));
    return false;
  }

  for (int Index = 0; Index < OutViewInfos.Num(); Index++)
  {
    FMinimalViewInfo& ViewInfo = OutViewInfos[Index];
    ViewInfo.PostProcessBlendWeight = 0.0f;
  }

  return true;
}

bool UComfyTexturesWidgetBase::CaptureSceneTextures(UWorld* World, TArray<AActor*> Actors, const TArray<FMinimalViewInfo>& ViewInfos, EComfyTexturesMode Mode, const TSharedPtr<TArray<FComfyTexturesCaptureOutput>>& Outputs) const
{
  if (World == nullptr)
  {
    UE_LOG(LogComfyTextures, Error, TEXT("World is null."));
    return false;
  }

  if (Actors.Num() == 0)
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Actors is empty."));
    return false;
  }

  if (ViewInfos.Num() == 0)
  {
    UE_LOG(LogComfyTextures, Error, TEXT("ViewInfos is empty."));
    return false;
  }

  if (Outputs == nullptr)
  {
    UE_LOG(LogComfyTextures, Error, TEXT("Outputs is null."));
    return false;
  }

  UComfyTexturesSettings* Settings = GetMutableDefault<UComfyTexturesSettings>();

  // create RTF RGBA8 render target
  UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>();
  RenderTarget->InitCustomFormat(Settings->CaptureSize, Settings->CaptureSize, EPixelFormat::PF_FloatRGBA, true);
  RenderTarget->UpdateResourceImmediate();

  // create scene capture component
  USceneCaptureComponent2D* SceneCapture = NewObject<USceneCaptureComponent2D>();
  SceneCapture->TextureTarget = RenderTarget;

  if (Mode == EComfyTexturesMode::Edit)
  {
    SceneCapture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
  }
  else
  {
    SceneCapture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
  }

  SceneCapture->ShowOnlyActors = Actors;
  SceneCapture->bCaptureEveryFrame = false;
  SceneCapture->bCaptureOnMovement = false;
  SceneCapture->bAlwaysPersistRenderingState = true;
  SceneCapture->RegisterComponentWithWorld(World);

  for (int Index = 0; Index < ViewInfos.Num(); Index++)
  {
    const FMinimalViewInfo& ViewInfo = ViewInfos[Index];

    SceneCapture->SetCameraView(ViewInfo);

    FComfyTexturesCaptureOutput Output;

    // capture the scene
    SceneCapture->CaptureSource = ESceneCaptureSource::SCS_SceneColorSceneDepth;
    SceneCapture->CaptureScene();

    if (!ReadRenderTargetPixels(RenderTarget, EComfyTexturesRenderTextureMode::Depth, Output.Depth))
    {
      UE_LOG(LogComfyTextures, Error, TEXT("Failed to read render target pixels."));
      return false;
    }

    if (!ReadRenderTargetPixels(RenderTarget, EComfyTexturesRenderTextureMode::RawDepth, Output.RawDepth))
    {
      UE_LOG(LogComfyTextures, Error, TEXT("Failed to read render target pixels."));
      return false;
    }

    SceneCapture->CaptureSource = ESceneCaptureSource::SCS_BaseColor;
    SceneCapture->CaptureScene();

    if (!ReadRenderTargetPixels(RenderTarget, EComfyTexturesRenderTextureMode::Color, Output.Color))
    {
      UE_LOG(LogComfyTextures, Error, TEXT("Failed to read render target pixels."));
      return false;
    }

    SceneCapture->CaptureSource = ESceneCaptureSource::SCS_Normal;
    SceneCapture->CaptureScene();

    if (!ReadRenderTargetPixels(RenderTarget, EComfyTexturesRenderTextureMode::Normals, Output.Normals))
    {
      UE_LOG(LogComfyTextures, Error, TEXT("Failed to read render target pixels."));
      return false;
    }

    Outputs->Add(MoveTemp(Output));
  }

  // destroy the scene capture component
  SceneCapture->DestroyComponent();
  SceneCapture = nullptr;

  // destroy the render target
  RenderTarget->ConditionalBeginDestroy();
  RenderTarget = nullptr;

  return true;
}

void UComfyTexturesWidgetBase::ProcessSceneTextures(const TSharedPtr<TArray<FComfyTexturesCaptureOutput>>& Outputs, EComfyTexturesMode Mode, int TargetSize, TFunction<void()> Callback) const
{
  TargetSize = FMath::RoundUpToPowerOfTwo(TargetSize);

  AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, TargetSize, Outputs, Mode, Callback]()
    {
      double StartTime = FPlatformTime::Seconds();

      for (int Index = 0; Index < Outputs->Num(); Index++)
      {
        FComfyTexturesCaptureOutput& Output = (*Outputs)[Index];

        if (Mode == EComfyTexturesMode::Edit)
        {
          Output.EditMask.Width = Output.Color.Width;
          Output.EditMask.Height = Output.Color.Height;
          CreateEditMaskFromImage(Output.Color.Pixels, Output.EditMask.Pixels);
          ResizeImage(Output.EditMask, TargetSize, TargetSize);
        }

        // create the edge mask
        CreateEdgeMask(Output.Depth, Output.Normals, Output.EdgeMask);
        ResizeImage(Output.EdgeMask, TargetSize, TargetSize);

        ResizeImage(Output.Color, TargetSize, TargetSize);
        ResizeImage(Output.Depth, TargetSize, TargetSize);
        ResizeImage(Output.Normals, TargetSize, TargetSize);
      }

      double EndTime = FPlatformTime::Seconds();

      UE_LOG(LogComfyTextures, Display, TEXT("Processed %d scene textures in %f seconds"), Outputs->Num(), EndTime - StartTime);

      AsyncTask(ENamedThreads::GameThread, Callback);
    });
}

void UComfyTexturesWidgetBase::ResizeImage(FComfyTexturesImageData& Image, int NewWidth, int NewHeight) const
{
  static TArray<FColor> OldPixels;
  OldPixels.SetNumUninitialized(Image.Width * Image.Height);

  for (int Y = 0; Y < Image.Height; Y++)
  {
    for (int X = 0; X < Image.Width; X++)
    {
      FLinearColor Pixel = Image.Pixels[Y * Image.Width + X];
      Pixel *= 255.0f;
      OldPixels[Y * Image.Width + X] = FColor(Pixel.R, Pixel.G, Pixel.B, Pixel.A);
    }
  }

  TArray<FColor> NewPixels;
  NewPixels.SetNumUninitialized(NewWidth * NewHeight);

  FImageUtils::ImageResize(Image.Width, Image.Height, OldPixels, NewWidth, NewHeight, NewPixels, true, false);

  Image.Width = NewWidth;
  Image.Height = NewHeight;
  Image.Pixels.SetNumUninitialized(NewWidth * NewHeight);

  for (int Y = 0; Y < Image.Height; Y++)
  {
    for (int X = 0; X < Image.Width; X++)
    {
      FColor Pixel = NewPixels[Y * Image.Width + X];
      FLinearColor NewPixel = FLinearColor(Pixel.R, Pixel.G, Pixel.B, Pixel.A);
      NewPixel /= 255.0f;
      Image.Pixels[Y * Image.Width + X] = NewPixel;
    }
  }
}
