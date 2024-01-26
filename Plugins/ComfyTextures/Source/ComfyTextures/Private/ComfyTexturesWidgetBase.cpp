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

  for (const TPair<int, FComfyTexturesRenderData>& Pair : RenderData)
  {
    if (Pair.Value.State == EComfyTexturesRenderState::Pending)
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
  for (const TPair<int, FComfyTexturesRenderData>& Pair : RenderData)
  {
    if (Pair.Value.State != EComfyTexturesRenderState::Finished)
    {
      return false;
    }

    if (Pair.Value.OutputFileNames.Num() == 0)
    {
      return false;
    }
  }

  return true;
}

bool UComfyTexturesWidgetBase::ProcessMultipleActors(const TArray<AActor*>& Actors, const FComfyTexturesRenderOptions& RenderOpts, const FComfyTexturesCameraOptions& CameraOpts)
{
  if (!IsConnected())
  {
    UE_LOG(LogTemp, Warning, TEXT("Not connected to ComfyUI"));
    return false;
  }

  if (State != EComfyTexturesState::Idle)
  {
    UE_LOG(LogTemp, Warning, TEXT("Not idle"));
    return false;
  }

  if (Actors.Num() == 0)
  {
    UE_LOG(LogTemp, Warning, TEXT("No actors to process"));
    return false;
  }

  State = EComfyTexturesState::Rendering;
  OnStateChanged(State);

  RenderData.Empty();
  PromptIdToRequestIndex.Empty();
  ActorSet = Actors;

  TArray<FMinimalViewInfo> ViewInfos;
  if (!CreateCameraTransforms(Actors[0], CameraOpts, ViewInfos))
  {
    UE_LOG(LogTemp, Warning, TEXT("Failed to create camera transforms"));
    TransitionToIdleState();
    return false;
  }

  UTexture2D* MagentaPixel = nullptr;
  TMap<AActor*, TPair<UMaterialInstanceDynamic*, UTexture*>> ActorToTextureMap;

  if (RenderOpts.Mode == EComfyTexturesMode::Edit && RenderOpts.EditMaskMode == EComfyTexturesEditMaskMode::FromObject)
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
        UE_LOG(LogTemp, Error, TEXT("Material is null for actor %s."), *Actor->GetName());
        continue;
      }

      // cast the material to a material instance dynamic
      UMaterialInstanceDynamic* MaterialInstance = Cast<UMaterialInstanceDynamic>(Material);

      // if the material is not a material instance dynamic, skip it
      if (MaterialInstance == nullptr)
      {
        UE_LOG(LogTemp, Error, TEXT("Material is not a material instance dynamic for actor %s."), *Actor->GetName());
        continue;
      }

      UTexture* OldTexture = nullptr;
      if (!MaterialInstance->GetTextureParameterValue(TEXT("BaseColor"), OldTexture))
      {
        UE_LOG(LogTemp, Error, TEXT("Failed to get parameter value \"BaseColor\" for actor %s."), *Actor->GetName());
        continue;
      }

      ActorToTextureMap.Add(Actor, TPair<UMaterialInstanceDynamic*, UTexture*>(MaterialInstance, OldTexture));

      // set the texture parameter to the magenta pixel texture
      MaterialInstance->SetTextureParameterValue(TEXT("BaseColor"), MagentaPixel);
    }
  }

  TArray<FComfyTexturesCaptureOutput> CaptureResults;

  if (!CaptureSceneTextures(Actors[0]->GetWorld(), Actors, ViewInfos, RenderOpts.Mode, CaptureResults))
  {
    UE_LOG(LogTemp, Warning, TEXT("Failed to capture input textures"));
    TransitionToIdleState();
    return false;
  }

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

  for (int Index = 0; Index < CaptureResults.Num(); Index++)
  {
    const FComfyTexturesCaptureOutput& Output = CaptureResults[Index];
    const FMinimalViewInfo& ViewInfo = ViewInfos[Index];
    const FComfyTexturesImageData& RawDepth = CaptureResults[Index].RawDepth;

    TArray<FComfyTexturesImageData> Images;
    TArray<FString> FileNames;

    Images.Add(Output.Depth);
    FileNames.Add("depth_" + FString::FromInt(Index) + ".png");

    Images.Add(Output.Normals);
    FileNames.Add("normals_" + FString::FromInt(Index) + ".png");

    Images.Add(Output.Color);
    FileNames.Add("color_" + FString::FromInt(Index) + ".png");

    Images.Add(Output.EditMask);
    FileNames.Add("mask_" + FString::FromInt(Index) + ".png");

    Images.Add(Output.EdgeMask);
    FileNames.Add("edge_mask_" + FString::FromInt(Index) + ".png");

    bool bSuccess = UploadImages(Images, FileNames, [this, RenderOpts, ViewInfo, RawDepth](const TArray<FString>& FileNames, bool bSuccess)
      {
        if (!bSuccess)
        {
          UE_LOG(LogTemp, Warning, TEXT("Upload failed"));
          TransitionToIdleState();
          return;
        }

        UE_LOG(LogTemp, Warning, TEXT("Upload complete"));

        for (const FString& FileName : FileNames)
        {
          UE_LOG(LogTemp, Warning, TEXT("Uploaded file: %s"), *FileName);
        }

        FComfyTexturesRenderOptions NewRenderOpts = RenderOpts;
        NewRenderOpts.DepthImageFilename = FileNames[0];
        NewRenderOpts.NormalsImageFilename = FileNames[1];
        NewRenderOpts.ColorImageFilename = FileNames[2];
        NewRenderOpts.MaskImageFilename = FileNames[3];
        NewRenderOpts.EdgeMaskImageFilename = FileNames[4];

        int RequestIndex;
        if (!QueueRender(NewRenderOpts, RequestIndex))
        {
          UE_LOG(LogTemp, Warning, TEXT("Failed to queue render"));
          TransitionToIdleState();
          return;
        }

        FComfyTexturesRenderData& Data = RenderData[RequestIndex];
        Data.ViewInfo = ViewInfo;

        TOptional<FMatrix> CustomProjectionMatrix;
        FMatrix ViewMatrix, ProjectionMatrix, ViewProjectionMatrix;
        UGameplayStatics::CalculateViewProjectionMatricesFromMinimalView(Data.ViewInfo, CustomProjectionMatrix,
          ViewMatrix, ProjectionMatrix, ViewProjectionMatrix);
        Data.ViewMatrix = ViewMatrix;
        Data.ProjectionMatrix = ProjectionMatrix;
        Data.RawDepth = RawDepth;
      });

    if (!bSuccess)
    {
      UE_LOG(LogTemp, Warning, TEXT("Failed to upload capture results"));
      TransitionToIdleState();
      return false;
    }
  }

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
  // for every iteration
  //   for every pixel
  //     if the pixel has alpha == 0.0
  //       if any of the pixel's neighbors have alpha > 0.0
  //         add 1 to the neighbor count
  //         set the pixel's alpha to 1.0
  //         set the pixel's color to the average of the pixel's neighbors

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
          Pixel.A = 255;
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

  // Edge functions
  auto Edge = [](const FVector2D& A, const FVector2D& B, const FVector2D& C)
    {
      return (C.X - A.X) * (B.Y - A.Y) - (C.Y - A.Y) * (B.X - A.X);
    };

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

bool ProcessRenderResultForActor(AActor* Actor, const TMap<int, FComfyTexturesRenderData>& RenderData, TFunction<void(bool)> Callback)
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
    UE_LOG(LogTemp, Error, TEXT("Static mesh is null for actor %s."), *Actor->GetName());
    return false;
  }

  // get the material of the static mesh
  UMaterialInterface* Material = StaticMeshComponent->GetMaterial(0);

  // if the material is null, skip it
  if (Material == nullptr)
  {
    UE_LOG(LogTemp, Error, TEXT("Material is null for actor %s."), *Actor->GetName());
    return false;
  }

  UTexture2D* Texture2D = nullptr;

  // get the texture assigned in the Tex param
  UTexture* Texture = nullptr;
  if (!Material->GetTextureParameterValue(TEXT("BaseColor"), Texture))
  {
    UE_LOG(LogTemp, Error, TEXT("Failed to get parameter value \"BaseColor\" for actor %s."), *Actor->GetName());
    return false;
  }

  // if the texture is null, skip it
  if (Texture == nullptr)
  {
    UE_LOG(LogTemp, Error, TEXT("Texture is null for actor %s."), *Actor->GetName());
    return false;
  }

  Texture2D = Cast<UTexture2D>(Texture);
  if (Texture2D == nullptr)
  {
    UE_LOG(LogTemp, Error, TEXT("Failed to create texture for actor %s."), *Actor->GetName());
    return false;
  }

  if (Texture2D->Source.GetNumMips() <= 0)
  {
    UE_LOG(LogTemp, Error, TEXT("No mipmaps available in texture for actor %s."), *Actor->GetName());
    return false;
  }

  int TextureWidth = Texture2D->GetSizeX();
  int TextureHeight = Texture2D->GetSizeY();

  // iterate the faces of the static mesh
  // 
  // Access the LOD (Level of Detail) data of the mesh
  const FStaticMeshLODResources& MeshLod = StaticMesh->GetLODForExport(0);

  // Access the index buffer
  TArray<uint32> Indices;
  MeshLod.IndexBuffer.GetCopy(Indices);

  int VertexCount = MeshLod.VertexBuffers.PositionVertexBuffer.GetNumVertices();

  TArray<FVector> Vertices;
  Vertices.SetNumUninitialized(VertexCount);

  TArray<FVector2D> Uvs;
  Uvs.SetNumUninitialized(VertexCount);

  for (int32 VertexIndex = 0; VertexIndex < VertexCount; VertexIndex++)
  {
    FVector Vertex = (FVector)MeshLod.VertexBuffers.PositionVertexBuffer.VertexPosition(VertexIndex);
    Vertices[VertexIndex] = Vertex;

    FVector2D Uv = (FVector2D)MeshLod.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(VertexIndex, 0);
    Uvs[VertexIndex] = Uv;
  }

  // get the first item from RenderData TMap
  const FComfyTexturesRenderData& Data = RenderData.begin().Value();
  const FMinimalViewInfo& ViewInfo = Data.ViewInfo;
  const FMatrix& ViewMatrix = Data.ViewMatrix;
  const FMatrix& ProjectionMatrix = Data.ProjectionMatrix;

  AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [=]()
    {
      FMatrix ViewProjectionMatrix = ViewMatrix * ProjectionMatrix;
      FIntRect ViewRect(0, 0, Data.OutputWidth, Data.OutputHeight);

      TArray<FColor> Pixels;
      Pixels.SetNumZeroed(TextureWidth * TextureHeight);

      // Iterate over the faces
      for (int32 FaceIndex = 0; FaceIndex < Indices.Num(); FaceIndex += 3)
      {
        // Each face is represented by 3 indices
        uint32 Index0 = Indices[FaceIndex];
        uint32 Index1 = Indices[FaceIndex + 1];
        uint32 Index2 = Indices[FaceIndex + 2];

        // get the vertices of the face
        const FVector& Vertex0 = Vertices[Index0];
        const FVector& Vertex1 = Vertices[Index1];
        const FVector& Vertex2 = Vertices[Index2];

        FVector FaceNormal = -FVector::CrossProduct(Vertex1 - Vertex0, Vertex2 - Vertex0).GetSafeNormal();
        FVector FaceNormalWorld = ActorTransform.TransformVector(FaceNormal);

        float Dot = 0.0f;
        if (ViewInfo.ProjectionMode == ECameraProjectionMode::Perspective)
        {
          FVector Vertex0World = ActorTransform.TransformPosition(Vertex0);
          Dot = FVector::DotProduct(FaceNormalWorld, (ViewInfo.Location - Vertex0World).GetSafeNormal());
        }
        else if (ViewInfo.ProjectionMode == ECameraProjectionMode::Orthographic)
        {
          // get forward vector of viewinfo
          FVector Forward = ViewInfo.Rotation.Vector();
          Dot = FVector::DotProduct(FaceNormalWorld, -Forward);
        }

        if (Dot <= 0.0f)
        {
          continue;
        }

        // get the UVs of the face
        const FVector2D& Uv0 = Uvs[Index0];
        const FVector2D& Uv1 = Uvs[Index1];
        const FVector2D& Uv2 = Uvs[Index2];

        RasterizeTriangle(Uv0, Uv1, Uv2, TextureWidth, TextureHeight, [&](int X, int Y, const FVector& Barycentric)
          {
            int PixelIndex = X + Y * TextureWidth;
            if (PixelIndex < 0 || PixelIndex >= Pixels.Num())
            {
              return;
            }

            // find the local position of the pixel
            FVector LocalPosition = Barycentric.X * Vertex0 + Barycentric.Y * Vertex1 + Barycentric.Z * Vertex2;
            FVector WorldPosition = ActorTransform.TransformPosition(LocalPosition);

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

            // calculate the pixel coordinates
            int PixelX = FMath::FloorToInt(Uv.X * (Data.RawDepth.Width - 1));
            int PixelY = FMath::FloorToInt(Uv.Y * (Data.RawDepth.Height - 1));

            float ClosestDepth = Data.RawDepth.Pixels[PixelX + PixelY * Data.RawDepth.Width].R;

            if (ViewInfo.ProjectionMode == ECameraProjectionMode::Perspective)
            {
              FVector ViewSpacePoint = ViewMatrix.TransformPosition(WorldPosition);

              float Eps = 5.0f;
              if (ViewSpacePoint.Z > ClosestDepth + Eps)
              {
                Pixels[PixelIndex] = FColor(0, 0, 0, 255);//FColor(255, 0, 255, 255);
                return;
              }
            }
            else
            {
              FVector4 ClipSpacePoint = ProjectionMatrix.TransformFVector4(FVector4(0.0f, 0.0f, ClosestDepth, 1.0f));

              // Step 3: Perform Perspective Division
              float DepthInNdc = ClipSpacePoint.Z / ClipSpacePoint.W;
              float Eps = 0.01f;
              if (PosInScreenSpace.Z < DepthInNdc - Eps)
              {
                Pixels[PixelIndex] = FColor(0, 0, 0, 255);//FColor(255, 0, 255, 255);
                return;
              }
            }

            // get the pixel color from the input texture
            FLinearColor Pixel = SampleBilinear(Data.OutputPixels, Data.OutputWidth, Data.OutputHeight, Uv);
            Pixel *= 255.0f;
            Pixels[PixelIndex] = FColor(Pixel.R, Pixel.G, Pixel.B, Pixel.A);
          });
      }

      ExpandTextureIslands(Pixels, TextureWidth, TextureHeight, 4);

      AsyncTask(ENamedThreads::GameThread, [=]()
        {
          UKismetSystemLibrary::TransactObject(Texture2D);

          FColor* MipData = (FColor*)Texture2D->Source.LockMip(0);
          if (MipData == nullptr)
          {
            UE_LOG(LogTemp, Error, TEXT("Failed to lock mip 0 for texture %s."), *Texture2D->GetName());
            Callback(false);
            return false;
          }

          FMemory::Memcpy(MipData, Pixels.GetData(), Pixels.Num() * sizeof(FColor));

          Texture2D->Source.UnlockMip(0);
          Texture2D->MarkPackageDirty();
          Texture2D->UpdateResource();

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
    UE_LOG(LogTemp, Warning, TEXT("Not all requests succeeded"));
    TransitionToIdleState();
    return false;
  }

  if (RenderData.Num() == 0)
  {
    UE_LOG(LogTemp, Warning, TEXT("No requests to process"));
    TransitionToIdleState();
    return false;
  }

  LoadRenderResultImages([this](bool bSuccess)
    {
      if (!bSuccess)
      {
        UE_LOG(LogTemp, Warning, TEXT("Failed to load render result images"));
        TransitionToIdleState();
        return;
      }

      if (UKismetSystemLibrary::BeginTransaction("ComfyTextures", FText::FromString("Comfy Textures Process Actors"), nullptr) != 0)
      {
        UE_LOG(LogTemp, Warning, TEXT("Failed to begin transaction"));
        TransitionToIdleState();
        return;
      }

      static TAtomic<int> PendingResults;
      PendingResults = 0;

      for (AActor* Actor : ActorSet)
      {
        if (ProcessRenderResultForActor(Actor, RenderData, [&](bool bSuccess)
          {
            if (!bSuccess)
            {
              UE_LOG(LogTemp, Warning, TEXT("Failed to process render result for actor %s"), *Actor->GetName());
            }

            PendingResults--;
            if (PendingResults == 0)
            {
              if (UKismetSystemLibrary::EndTransaction() != -1)
              {
                UE_LOG(LogTemp, Warning, TEXT("Failed to end transaction"));
              }

              TransitionToIdleState();
            }

            return true;
          }))
        {
          PendingResults++;
        }
      }

      State = EComfyTexturesState::Processing;
      OnStateChanged(State);
    });

  return true;
}

void UComfyTexturesWidgetBase::CancelJob()
{
  if (State != EComfyTexturesState::Rendering)
  {
    UE_LOG(LogTemp, Warning, TEXT("Not rendering"));
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

    TSharedPtr<FJsonObject> Meta = (*NodeObject)->GetObjectField("_meta");
    if (!Meta.IsValid())
    {
      continue;
    }

    FString NodeTitle;
    if (!Meta->TryGetStringField("title", NodeTitle))
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

void SetNodeInputProperty(FJsonObject& Workflow, const FString& NodeName, const FString& PropertyName, double Value)
{
  TArray<TSharedPtr<FJsonObject>> Nodes = FindNodesByTitle(Workflow, NodeName);

  for (TSharedPtr<FJsonObject>& Node : Nodes)
  {
    if (!Node->HasField("inputs"))
    {
      continue;
    }

    Node->GetObjectField("inputs")->SetNumberField(PropertyName, Value);
  }
}

void SetNodeInputProperty(FJsonObject& Workflow, const FString& NodeName, const FString& PropertyName, int Value)
{
  TArray<TSharedPtr<FJsonObject>> Nodes = FindNodesByTitle(Workflow, NodeName);

  for (TSharedPtr<FJsonObject>& Node : Nodes)
  {
    if (!Node->HasField("inputs"))
    {
      continue;
    }

    Node->GetObjectField("inputs")->SetNumberField(PropertyName, Value);
  }
}

void SetNodeInputProperty(FJsonObject& Workflow, const FString& NodeName, const FString& PropertyName, const FString& Value)
{
  TArray<TSharedPtr<FJsonObject>> Nodes = FindNodesByTitle(Workflow, NodeName);

  for (TSharedPtr<FJsonObject>& Node : Nodes)
  {
    if (!Node->HasField("inputs"))
    {
      UE_LOG(LogTemp, Warning, TEXT("Node %s does not have inputs"), *NodeName);
      continue;
    }

    if (!Node->GetObjectField("inputs")->HasField(PropertyName))
    {
      UE_LOG(LogTemp, Warning, TEXT("Node %s does not have input %s"), *NodeName, *PropertyName);
      continue;
    }

    Node->GetObjectField("inputs")->SetStringField(PropertyName, Value);
  }
}

bool GetNodeInputProperty(FJsonObject& Workflow, const FString& NodeName, const FString& PropertyName, int& OutValue)
{
  TArray<TSharedPtr<FJsonObject>> Nodes = FindNodesByTitle(Workflow, NodeName);

  for (TSharedPtr<FJsonObject>& Node : Nodes)
  {
    if (!Node->HasField("inputs"))
    {
      continue;
    }

    if (!Node->GetObjectField("inputs")->TryGetNumberField(PropertyName, OutValue))
    {
      continue;
    }

    return true;
  }

  return false;
}

bool GetNodeInputProperty(FJsonObject& Workflow, const FString& NodeName, const FString& PropertyName, FString& OutValue)
{
  TArray<TSharedPtr<FJsonObject>> Nodes = FindNodesByTitle(Workflow, NodeName);

  for (TSharedPtr<FJsonObject>& Node : Nodes)
  {
    if (!Node->HasField("inputs"))
    {
      continue;
    }

    if (!Node->GetObjectField("inputs")->TryGetStringField(PropertyName, OutValue))
    {
      continue;
    }

    return true;
  }

  return false;
}

bool GetNodeInputProperty(FJsonObject& Workflow, const FString& NodeName, const FString& PropertyName, float& OutValue)
{
  TArray<TSharedPtr<FJsonObject>> Nodes = FindNodesByTitle(Workflow, NodeName);

  for (TSharedPtr<FJsonObject>& Node : Nodes)
  {
    if (!Node->HasField("inputs"))
    {
      continue;
    }

    if (!Node->GetObjectField("inputs")->TryGetNumberField(PropertyName, OutValue))
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
    UE_LOG(LogTemp, Warning, TEXT("Not connected to ComfyUI"));
    return false;
  }

  if (!FPaths::FileExists(RenderOpts.WorkflowJsonPath))
  {
    UE_LOG(LogTemp, Warning, TEXT("Workflow JSON file does not exist: %s"), *RenderOpts.WorkflowJsonPath);
    return false;
  }

  FString JsonString = "";
  if (!FFileHelper::LoadFileToString(JsonString, *RenderOpts.WorkflowJsonPath))
  {
    UE_LOG(LogTemp, Warning, TEXT("Failed to load workflow JSON file: %s"), *RenderOpts.WorkflowJsonPath);
    return false;
  }

  TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
  TSharedPtr<FJsonObject> Workflow;

  if (!FJsonSerializer::Deserialize(Reader, Workflow))
  {
    UE_LOG(LogTemp, Warning, TEXT("Failed to deserialize workflow JSON"));
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

  TSharedPtr<FJsonObject> RequestBody = MakeShareable(new FJsonObject);
  RequestBody->SetStringField("client_id", HttpClient->ClientId);
  RequestBody->SetObjectField("prompt", Workflow);

  FString RequestBodyString;
  TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBodyString);
  FJsonSerializer::Serialize(RequestBody.ToSharedRef(), Writer);

  UE_LOG(LogTemp, Warning, TEXT("Sending render request: %s"), *RequestBodyString);

  RequestIndex = NextRequestIndex++;
  RenderData.Add(RequestIndex, FComfyTexturesRenderData());

  TWeakObjectPtr<UComfyTexturesWidgetBase> WeakThis(this);

  return HttpClient->DoHttpPostRequest("prompt", RequestBodyString, [WeakThis, RequestIndex](const TSharedPtr<FJsonObject>& Response, bool bWasSuccessful)
    {
      if (!WeakThis.IsValid())
      {
        return;
      }

      UComfyTexturesWidgetBase* This = WeakThis.Get();
      FComfyTexturesRenderData& Data = This->RenderData[RequestIndex];

      if (!bWasSuccessful)
      {
        UE_LOG(LogTemp, Warning, TEXT("Failed to send render request"));
        Data.State = EComfyTexturesRenderState::Failed;
        This->HandleRenderStateChanged(Data);
        return;
      }

      FString PromptId = Response->GetStringField("prompt_id");
      Data.PromptId = PromptId;
      Data.State = EComfyTexturesRenderState::Pending;

      if (Response->HasField("error"))
      {
        UE_LOG(LogTemp, Warning, TEXT("Render request failed"));
        Data.State = EComfyTexturesRenderState::Failed;
      }
      else
      {
        UE_LOG(LogTemp, Warning, TEXT("Render request successful"));
      }

      This->PromptIdToRequestIndex.Add(PromptId, RequestIndex);
      This->HandleRenderStateChanged(Data);
    });
}

void UComfyTexturesWidgetBase::InterruptRender() const
{
  if (!IsConnected())
  {
    UE_LOG(LogTemp, Warning, TEXT("Not connected to ComfyUI"));
    return;
  }

  TSharedPtr<FJsonObject> RequestBody = MakeShareable(new FJsonObject);

  FString RequestBodyString;
  TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBodyString);
  FJsonSerializer::Serialize(RequestBody.ToSharedRef(), Writer);

  UE_LOG(LogTemp, Warning, TEXT("Sending interrupt request: %s"), *RequestBodyString);

  HttpClient->DoHttpPostRequest("interrupt", RequestBodyString, [this](const TSharedPtr<FJsonObject>& Response, bool bWasSuccessful)
    {
      if (!bWasSuccessful)
      {
        UE_LOG(LogTemp, Warning, TEXT("Failed to send interrupt request"));
        return;
      }

      UE_LOG(LogTemp, Warning, TEXT("Interrupt request successful"));
    });
}

void UComfyTexturesWidgetBase::ClearRenderQueue()
{
  if (!IsConnected())
  {
    UE_LOG(LogTemp, Warning, TEXT("Not connected to ComfyUI"));
    return;
  }

  TSharedPtr<FJsonObject> RequestBody = MakeShareable(new FJsonObject);
  RequestBody->SetBoolField("clear", true);

  FString RequestBodyString;
  TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBodyString);
  FJsonSerializer::Serialize(RequestBody.ToSharedRef(), Writer);

  UE_LOG(LogTemp, Warning, TEXT("Sending clear request: %s"), *RequestBodyString);

  HttpClient->DoHttpPostRequest("queue", RequestBodyString, [this](const TSharedPtr<FJsonObject>& Response, bool bWasSuccessful)
    {
      if (!bWasSuccessful)
      {
        UE_LOG(LogTemp, Warning, TEXT("Failed to send clear request"));
        return;
      }

      UE_LOG(LogTemp, Warning, TEXT("Clear request successful"));
    });
}

void UComfyTexturesWidgetBase::FreeComfyMemory(bool bUnloadModels)
{
  if (!IsConnected())
  {
    UE_LOG(LogTemp, Warning, TEXT("Not connected to ComfyUI"));
    return;
  }

  TSharedPtr<FJsonObject> RequestBody = nullptr;
  FString RequestBodyString;

  if (bUnloadModels)
  {
    RequestBody = MakeShareable(new FJsonObject);
    RequestBody->SetBoolField("free_memory", true);
    RequestBody->SetBoolField("unload_models", true);

    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBodyString);
    FJsonSerializer::Serialize(RequestBody.ToSharedRef(), Writer);

    UE_LOG(LogTemp, Warning, TEXT("Sending free memory request: %s"), *RequestBodyString);

    HttpClient->DoHttpPostRequest("free", RequestBodyString, [this](const TSharedPtr<FJsonObject>& Response, bool bWasSuccessful)
      {
        if (!bWasSuccessful)
        {
          UE_LOG(LogTemp, Warning, TEXT("Failed to send cleanup request"));
          return;
        }

        UE_LOG(LogTemp, Warning, TEXT("Cleanup request successful"));
      });
  }

  RequestBody = MakeShareable(new FJsonObject);
  RequestBody->SetBoolField("clear", true);

  RequestBodyString = "";
  TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBodyString);
  FJsonSerializer::Serialize(RequestBody.ToSharedRef(), Writer);

  UE_LOG(LogTemp, Warning, TEXT("Sending history clear request: %s"), *RequestBodyString);

  HttpClient->DoHttpPostRequest("history", RequestBodyString, [this](const TSharedPtr<FJsonObject>& Response, bool bWasSuccessful)
    {
      if (!bWasSuccessful)
      {
        UE_LOG(LogTemp, Warning, TEXT("Failed to send history clear request"));
        return;
      }

      UE_LOG(LogTemp, Warning, TEXT("History clear request successful"));
    });
}

bool UComfyTexturesWidgetBase::ReadTextFile(FString FilePath, FString& OutText) const
{
  return FFileHelper::LoadFileToString(OutText, *FilePath);
}

bool UComfyTexturesWidgetBase::PrepareActors(const TArray<AActor*>& Actors, const FComfyTexturesPrepareOptions& PrepareOpts)
{
  if (Actors.Num() == 0)
  {
    UE_LOG(LogTemp, Warning, TEXT("No actors to prepare"));
    return false;
  }

  if (PrepareOpts.BaseMaterial == nullptr)
  {
    UE_LOG(LogTemp, Warning, TEXT("Base material is null"));
    return false;
  }

  // get pixels of reference texture
  UTexture2D* ReferenceTexture = PrepareOpts.ReferenceTexture;
  if (ReferenceTexture == nullptr)
  {
    UE_LOG(LogTemp, Warning, TEXT("Reference texture is null"));
    return false;
  }

  void* ReferenceMipData = ReferenceTexture->Source.LockMip(0);
  if (ReferenceMipData == nullptr)
  {
    UE_LOG(LogTemp, Error, TEXT("Failed to lock texture mip data"));
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
      UE_LOG(LogTemp, Error, TEXT("Failed to calculate screen bounds for actor %s."), *Actor->GetName());
      continue;
    }

    FVector2D SizeOnScreen = ActorScreenBounds.GetSize();
    float LargerSize = FMath::Max(SizeOnScreen.X, SizeOnScreen.Y);

    // get next power of two
    int TextureSize = FMath::Lerp((float)Settings->MinTextureSize, (float)Settings->MaxTextureSize, LargerSize);
    TextureSize = (float)TextureSize * Settings->TextureQualityMultiplier;

    // clamp to max texture size
    TextureSize = FMath::Clamp(TextureSize, Settings->MinTextureSize, Settings->MaxTextureSize);
    TextureSize = FMath::RoundUpToPowerOfTwo(TextureSize);

    UE_LOG(LogTemp, Warning, TEXT("Chosen texture size: %d for actor %s."), TextureSize, *Actor->GetName());

    FString TextureName = "GeneratedTexture_" + Id;

    // rescale reference texture to TextureSize
    TArray<FColor> RescaledReferencePixels;
    RescaledReferencePixels.SetNumZeroed(TextureSize * TextureSize);

    FImageUtils::ImageResize(ReferenceTexture->GetSizeX(), ReferenceTexture->GetSizeY(), ReferencePixels, TextureSize, TextureSize, RescaledReferencePixels, false);

    UTexture2D* Texture2D = CreateTexture2D(TextureSize, TextureSize, RescaledReferencePixels);
    if (Texture2D == nullptr)
    {
      UE_LOG(LogTemp, Error, TEXT("Failed to create texture %s"), *TextureName);
      return false;
    }

    Texture2D->Rename(*TextureName);

    if (!CreateAssetPackage(Texture2D, "/Game/Textures/Generated/"))
    {
      UE_LOG(LogTemp, Error, TEXT("Failed to create asset package for texture %s"), *TextureName);
      return false;
    }

    // create a new material instance
    UMaterialInstanceDynamic* MaterialInstance = UMaterialInstanceDynamic::Create(PrepareOpts.BaseMaterial, nullptr);
    MaterialInstance->SetTextureParameterValue(TEXT("BaseColor"), Texture2D);

    FString MaterialName = "GeneratedMaterial_" + Id;
    MaterialInstance->Rename(*MaterialName);

    if (!CreateAssetPackage(MaterialInstance, "/Game/Materials/Generated/"))
    {
      UE_LOG(LogTemp, Error, TEXT("Failed to create asset package for material instance %s"), *MaterialName);
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
    UE_LOG(LogTemp, Warning, TEXT("Failed to load workflow JSON file: %s"), *JsonPath);
    return false;
  }

  TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
  TSharedPtr<FJsonObject> Workflow;

  if (!FJsonSerializer::Deserialize(Reader, Workflow))
  {
    UE_LOG(LogTemp, Warning, TEXT("Failed to deserialize workflow JSON"));
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
    UE_LOG(LogTemp, Warning, TEXT("Invalid mode"));
  }

  return JsonPath;
}

void UComfyTexturesWidgetBase::SetEditorFrameRate(int Fps)
{
  GEditor->SetMaxFPS(Fps);
}

void GetChildActorsRecursive(AActor* Actor, TSet<AActor*>& OutActors)
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
    UE_LOG(LogTemp, Warning, TEXT("Websocket message missing type field"));
    return;
  }

  const TSharedPtr<FJsonObject>* MessageData;
  if (!Message->TryGetObjectField("data", MessageData))
  {
    UE_LOG(LogTemp, Warning, TEXT("Websocket message missing data field"));
    return;
  }

  FString PromptId;
  if (!(*MessageData)->TryGetStringField("prompt_id", PromptId))
  {
    UE_LOG(LogTemp, Warning, TEXT("Websocket message missing prompt_id field"));
    return;
  }

  if (!PromptIdToRequestIndex.Contains(PromptId))
  {
    UE_LOG(LogTemp, Warning, TEXT("Received websocket message for unknown prompt_id: %s"), *PromptId);
    return;
  }

  int RequestIndex = PromptIdToRequestIndex[PromptId];
  if (!RenderData.Contains(RequestIndex))
  {
    UE_LOG(LogTemp, Warning, TEXT("Received websocket message for unknown request index: %d"), RequestIndex);
    return;
  }

  FComfyTexturesRenderData& Data = RenderData[RequestIndex];

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
      UE_LOG(LogTemp, Warning, TEXT("Websocket message missing value field"));
      return;
    }

    float Max;
    if (!(*MessageData)->TryGetNumberField("max", Max))
    {
      UE_LOG(LogTemp, Warning, TEXT("Websocket message missing max field"));
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
      UE_LOG(LogTemp, Warning, TEXT("Websocket message missing output field"));
      return;
    }

    // "output": {"images": [{"filename": "ComfyUI_00062_.png", "subfolder": "", "type": "output"}]}, "prompt_id": "30840158-3b72-4c3e-9112-7efea0a3a2a8"}
    const TArray<TSharedPtr<FJsonValue>>* Images;

    if (!(*OutputData)->TryGetArrayField("images", Images))
    {
      UE_LOG(LogTemp, Warning, TEXT("Websocket message missing images field"));
      return;
    }

    for (const TSharedPtr<FJsonValue>& Image : *Images)
    {
      const TSharedPtr<FJsonObject>* ImageObject;
      if (!Image->TryGetObject(ImageObject))
      {
        UE_LOG(LogTemp, Warning, TEXT("Websocket message missing image object"));
        return;
      }

      FString Filename;
      if (!(*ImageObject)->TryGetStringField("filename", Filename))
      {
        UE_LOG(LogTemp, Warning, TEXT("Websocket message missing filename field"));
        return;
      }

      FString Subfolder;
      if (!(*ImageObject)->TryGetStringField("subfolder", Subfolder))
      {
        UE_LOG(LogTemp, Warning, TEXT("Websocket message missing subfolder field"));
        return;
      }

      FString Type;
      if (!(*ImageObject)->TryGetStringField("type", Type))
      {
        UE_LOG(LogTemp, Warning, TEXT("Websocket message missing type field"));
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
    UE_LOG(LogTemp, Warning, TEXT("Unknown websocket message type: %s"), *MessageType);
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

  TArray<FFloat16Color> Pixels;
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

    UE_LOG(LogTemp, Warning, TEXT("MinDepth: %f, MaxDepth: %f"), MinDepth, MaxDepth);

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
    UE_LOG(LogTemp, Warning, TEXT("Unknown render texture mode: %d"), (int)Mode);
    return false;
  }

  return true;
}

bool UComfyTexturesWidgetBase::ConvertImageToPng(const FComfyTexturesImageData& Image, TArray<uint8>& OutBytes) const
{
  UE_LOG(LogTemp, Warning, TEXT("Converting image to PNG with Width: %d, Height: %d"), Image.Width, Image.Height);

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

  FImageUtils::CompressImageArray(Image.Width, Image.Height, Image8, OutBytes);
  return true;
}

bool UComfyTexturesWidgetBase::UploadImages(const TArray<FComfyTexturesImageData>& Images, const TArray<FString>& FileNames, TFunction<void(const TArray<FString>&, bool)> Callback) const
{
  if (Images.Num() != FileNames.Num())
  {
    UE_LOG(LogTemp, Warning, TEXT("Image and filename count do not match"));
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
        TArray<uint8> PngData;
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
              UE_LOG(LogTemp, Warning, TEXT("Failed to upload image"));
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
                UE_LOG(LogTemp, Warning, TEXT("Failed to get image url"));
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
        UE_LOG(LogTemp, Warning, TEXT("Failed to download image"));
        Callback(Pixels, 0, 0, false);
        return;
      }

      // Create an image wrapper using the PNG format
      IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
      TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

      // Set the compressed data for the image wrapper
      if (!ImageWrapper.IsValid() || !ImageWrapper->SetCompressed(PngData.GetData(), PngData.Num()))
      {
        UE_LOG(LogTemp, Warning, TEXT("Failed to decompress image"));
        Callback(Pixels, 0, 0, false);
        return;
      }

      // Decompress the image data
      TArray<uint8> RawData;
      if (!ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, RawData))
      {
        UE_LOG(LogTemp, Warning, TEXT("Failed to decompress image"));
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

bool UComfyTexturesWidgetBase::GenerateMipMaps(UTexture2D* Texture) const
{
  if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.Num() == 0)
  {
    return false;
  }

  FTexture2DMipMap* Mip0 = &Texture->GetPlatformData()->Mips[0];
  int32 SrcWidth = Mip0->SizeX;
  int32 SrcHeight = Mip0->SizeY;

  // if theres more than one mip, delete all but the first

  if (Texture->GetPlatformData()->Mips.Num() > 1)
  {
    for (int32 MipIndex = 1; MipIndex < Texture->GetPlatformData()->Mips.Num(); ++MipIndex)
    {
      FTexture2DMipMap* Mip = &Texture->GetPlatformData()->Mips[MipIndex];
      Mip->BulkData.RemoveBulkData();
    }

    // remove all but the first mip
    for (int32 MipIndex = Texture->GetPlatformData()->Mips.Num() - 1; MipIndex > 0; --MipIndex)
    {
      // free the mip data
      FTexture2DMipMap* Mip = &Texture->GetPlatformData()->Mips[MipIndex];
      Mip->BulkData.RemoveBulkData();

      // remove the mip from the array
      Texture->GetPlatformData()->Mips.RemoveAt(MipIndex);
    }
  }

  TArray<FColor> SrcData;
  SrcData.AddUninitialized(SrcWidth * SrcHeight);
  FMemory::Memcpy(SrcData.GetData(), Mip0->BulkData.Lock(LOCK_READ_ONLY), SrcWidth * SrcHeight * 4);
  Mip0->BulkData.Unlock();

  for (int32 MipIndex = 1; SrcWidth > 1 || SrcHeight > 1; ++MipIndex)
  {
    SrcWidth = FMath::Max(SrcWidth / 2, 1);
    SrcHeight = FMath::Max(SrcHeight / 2, 1);

    TArray<FColor> NewMipData;
    NewMipData.AddUninitialized(SrcWidth * SrcHeight);

    FImageUtils::ImageResize(SrcWidth * 2, SrcHeight * 2, TArrayView<const FColor>(SrcData), SrcWidth, SrcHeight, TArrayView<FColor>(NewMipData), false);

    FTexture2DMipMap* NewMip = new FTexture2DMipMap();
    NewMip->SizeX = SrcWidth;
    NewMip->SizeY = SrcHeight;

    NewMip->BulkData.Lock(LOCK_READ_WRITE);
    NewMip->BulkData.Realloc(SrcWidth * SrcHeight * 4);
    NewMip->BulkData.Unlock();

    FMemory::Memcpy(NewMip->BulkData.Lock(LOCK_READ_WRITE), NewMipData.GetData(), SrcWidth * SrcHeight * 4);
    NewMip->BulkData.Unlock();

    Texture->GetPlatformData()->Mips.Add(NewMip);

    SrcData = MoveTemp(NewMipData);
  }

  Texture->UpdateResource();

  return true;
}

// calculate the approximate screen bounds of an actor using the actor bounds and the camera view info
bool UComfyTexturesWidgetBase::CalculateApproximateScreenBounds(AActor* Actor, const FMinimalViewInfo& ViewInfo, FBox2D& OutBounds) const
{
  if (Actor == nullptr)
  {
    UE_LOG(LogTemp, Error, TEXT("Actor is null."));
    return false;
  }

  FBox ActorBounds = Actor->GetComponentsBoundingBox();
  FVector ActorCenter = ActorBounds.GetCenter();
  FVector ActorExtent = ActorBounds.GetExtent();
  FTransform ActorTransform = Actor->GetTransform();

  FVector Corners[8];
  Corners[0] = ActorTransform.TransformPosition(ActorCenter + FVector(ActorExtent.X, ActorExtent.Y, ActorExtent.Z));
  Corners[1] = ActorTransform.TransformPosition(ActorCenter + FVector(ActorExtent.X, ActorExtent.Y, -ActorExtent.Z));
  Corners[2] = ActorTransform.TransformPosition(ActorCenter + FVector(ActorExtent.X, -ActorExtent.Y, ActorExtent.Z));
  Corners[3] = ActorTransform.TransformPosition(ActorCenter + FVector(ActorExtent.X, -ActorExtent.Y, -ActorExtent.Z));
  Corners[4] = ActorTransform.TransformPosition(ActorCenter + FVector(-ActorExtent.X, ActorExtent.Y, ActorExtent.Z));
  Corners[5] = ActorTransform.TransformPosition(ActorCenter + FVector(-ActorExtent.X, ActorExtent.Y, -ActorExtent.Z));
  Corners[6] = ActorTransform.TransformPosition(ActorCenter + FVector(-ActorExtent.X, -ActorExtent.Y, ActorExtent.Z));
  Corners[7] = ActorTransform.TransformPosition(ActorCenter + FVector(-ActorExtent.X, -ActorExtent.Y, -ActorExtent.Z));

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
    UE_LOG(LogTemp, Error, TEXT("Pixels.Num() != Width * Height"));
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

  if (GenerateMipMaps(Texture2D))
  {
    UE_LOG(LogTemp, Warning, TEXT("Generated mipmaps for texture %s"), *Texture2D->GetName());
  }
  else
  {
    UE_LOG(LogTemp, Warning, TEXT("Failed to generate mipmaps for texture %s"), *Texture2D->GetName());
  }

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
    UE_LOG(LogTemp, Error, TEXT("Asset is null."));
    return false;
  }

  if (PackagePath.Len() == 0)
  {
    UE_LOG(LogTemp, Error, TEXT("Package path is empty."));
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

  UE_LOG(LogTemp, Warning, TEXT("Saving asset %s to %s"), *Asset->GetName(), *PackageFileName);

  if (!UPackage::SavePackage(Package, Asset, *PackageFileName, SaveArgs))
  {
    UE_LOG(LogTemp, Error, TEXT("Failed to save package to %s"), *PackageFileName);
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

void UComfyTexturesWidgetBase::CreateEdgeMask(const FComfyTexturesImageData& Depth, const FComfyTexturesImageData& Normals, FComfyTexturesImageData& OutEdgeMask) const
{
  if (Depth.Width != Normals.Width || Depth.Height != Normals.Height)
  {
    UE_LOG(LogTemp, Error, TEXT("Depth and normals images have different dimensions."));
    return;
  }

  // perform edge detection on the depth and normals images
  OutEdgeMask.Width = Depth.Width;
  OutEdgeMask.Height = Depth.Height;
  OutEdgeMask.Pixels.SetNumUninitialized(Depth.Pixels.Num());

  // Assuming Depth and Normals are of the same dimensions
  for (int Y = 0; Y < Depth.Height; Y++)
  {
    for (int X = 0; X < Depth.Width; X++)
    {
      // Compute depth gradient
      float DepthGradient = ComputeDepthGradient(Depth, X, Y);

      // Compute normals gradient
      float NormalsGradient = ComputeNormalsGradient(Normals, X, Y);

      // Combine gradients to detect edges
      float EdgeStrength = FMath::Max(DepthGradient, NormalsGradient);

      // Set the pixel value in the output mask
      OutEdgeMask.Pixels[Y * Depth.Width + X] = FLinearColor(EdgeStrength, EdgeStrength, EdgeStrength, 1.0f);
    }
  }
}

float UComfyTexturesWidgetBase::ComputeDepthGradient(const FComfyTexturesImageData& Image, int X, int Y) const
{
  // Sobel operator kernels for x and y directions
  const int SobelX[3][3] = { {-1, 0, 1}, {-2, 0, 2}, {-1, 0, 1} };
  const int SobelY[3][3] = { {-1, -2, -1}, {0, 0, 0}, {1, 2, 1} };

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

float UComfyTexturesWidgetBase::ComputeNormalsGradient(const FComfyTexturesImageData& Image, int X, int Y) const
{
  // Sobel operator kernels for x and y directions
  const int SobelX[3][3] = { {-1, 0, 1}, {-2, 0, 2}, {-1, 0, 1} };
  const int SobelY[3][3] = { {-1, -2, -1}, {0, 0, 0}, {1, 2, 1} };

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

void UComfyTexturesWidgetBase::LoadRenderResultImages(TFunction<void(bool)> Callback)
{
  // Shared state for tracking task completion and results
  struct SharedState
  {
    int32 RemainingTasks;
    FThreadSafeBool bAllSuccessful = true;
  };
  TSharedPtr<SharedState> StateData = MakeShared<SharedState>();
  StateData->RemainingTasks = RenderData.Num();

  for (TPair<int, FComfyTexturesRenderData>& Pair : RenderData)
  {
    int Index = Pair.Key;
    FComfyTexturesRenderData& Data = Pair.Value;

    Async(EAsyncExecution::ThreadPool, [this, FileName = Data.OutputFileNames[0], StateData, Index, Callback]()
      {
        bool bSuccess = DownloadImage(FileName, [this, FileName, StateData, Index, Callback](TArray<FColor> Pixels, int Width, int Height, bool bWasSuccessful)
          {
            if (!bWasSuccessful)
            {
              UE_LOG(LogTemp, Warning, TEXT("Failed to download image %s"), *FileName);
              StateData->bAllSuccessful = false;
            }
            else
            {
              RenderData[Index].OutputPixels = Pixels;
              RenderData[Index].OutputWidth = Width;
              RenderData[Index].OutputHeight = Height;
            }

            // Check if this is the last task
            if (--StateData->RemainingTasks == 0)
            {
              Callback(StateData->bAllSuccessful);
            }
          });

        if (!bSuccess)
        {
          UE_LOG(LogTemp, Warning, TEXT("Failed to download image"));
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
  RenderData.Empty();
  PromptIdToRequestIndex.Empty();
  ActorSet.Empty();

  State = EComfyTexturesState::Idle;
  OnStateChanged(State);
}

bool UComfyTexturesWidgetBase::CreateCameraTransforms(AActor* Actor, const FComfyTexturesCameraOptions& CameraOptions, TArray<FMinimalViewInfo>& OutViewInfos) const
{
  if (Actor == nullptr)
  {
    UE_LOG(LogTemp, Error, TEXT("Actor is null."));
    return false;
  }

  OutViewInfos.Empty();

  if (CameraOptions.CameraMode == EComfyTexturesCameraMode::EditorCamera)
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
    ViewInfo.ProjectionMode = ECameraProjectionMode::Perspective;
    ViewInfo.AspectRatio = EditorViewportClient->AspectRatio;
    ViewInfo.PerspectiveNearClipPlane = EditorViewportClient->GetNearClipPlane();
    ViewInfo.PostProcessBlendWeight = 0.0f;

    OutViewInfos.Add(ViewInfo);
  }
  else if (CameraOptions.CameraMode == EComfyTexturesCameraMode::ExistingCamera)
  {
    if (CameraOptions.ExistingCamera == nullptr)
    {
      UE_LOG(LogTemp, Error, TEXT("Existing camera is null."));
      return false;
    }

    // get camera component from camera actor

    UCameraComponent* CameraComponent = CameraOptions.ExistingCamera->FindComponentByClass<UCameraComponent>();
    if (CameraComponent == nullptr)
    {
      UE_LOG(LogTemp, Error, TEXT("Camera component not found."));
      return false;
    }

    FMinimalViewInfo ViewInfo;
    CameraComponent->GetCameraView(0.0f, ViewInfo);

    OutViewInfos.Add(ViewInfo);
  }
  else if (CameraOptions.CameraMode == EComfyTexturesCameraMode::EightSides)
  {
    // get the actor bounds
    FBox ActorBounds = Actor->GetComponentsBoundingBox();

    // get the actor center
    FVector ActorCenter = ActorBounds.GetCenter();

    // get the actor extent
    FVector ActorExtent = ActorBounds.GetExtent();

    // create a transform for each side of the actor

    float CameraFov = 75.0f;
    float GreatestExtent = FMath::Max(ActorExtent.X, FMath::Max(ActorExtent.Y, ActorExtent.Z));
    float Distance = GreatestExtent / FMath::Tan(CameraFov * (float)PI / 360.0f);
    Distance *= 2.0f;

    /*FMinimalViewInfo FrontViewInfo;
    FrontViewInfo.Location = ActorCenter + FVector(Distance, 0.0f, 0.0f);
    FrontViewInfo.Rotation = FQuat::MakeFromEuler(FVector(0.0f, 0.0f, 180.0f));
    FrontViewInfo.FOV = CameraFov;
    FrontViewInfo.ProjectionMode = ECameraProjectionMode::Perspective;
    FrontViewInfo.AspectRatio = 1.0f;
    FrontViewInfo.PerspectiveNearClipPlane = 0.1f;
    FrontViewInfo.PostProcessBlendWeight = 0.0f;
    OutViewInfos.Add(FrontViewInfo);*/

    /*FTransform FrontTransform;
    FrontTransform.SetLocation(ActorCenter + FVector(Distance, 0.0f, 0.0f));
    FrontTransform.SetRotation(FQuat::MakeFromEuler(FVector(0.0f, 0.0f, 180.0f)));
    OutCameraTransforms.Add(FrontTransform);

    FTransform BackTransform;
    BackTransform.SetLocation(ActorCenter + FVector(-Distance, 0.0f, 0.0f));
    BackTransform.SetRotation(FQuat::MakeFromEuler(FVector(0.0f, 0.0f, 0.0f)));
    OutCameraTransforms.Add(BackTransform);

    FTransform LeftTransform;
    LeftTransform.SetLocation(ActorCenter + FVector(0.0f, Distance, 0.0f));
    LeftTransform.SetRotation(FQuat::MakeFromEuler(FVector(0.0f, 0.0f, -90.0f)));
    OutCameraTransforms.Add(LeftTransform);

    FTransform RightTransform;
    RightTransform.SetLocation(ActorCenter + FVector(0.0f, -Distance, 0.0f));
    RightTransform.SetRotation(FQuat::MakeFromEuler(FVector(0.0f, 0.0f, 90.0f)));
    OutCameraTransforms.Add(RightTransform);

    FTransform TopTransform;
    TopTransform.SetLocation(ActorCenter + FVector(0.0f, 0.0f, Distance));
    TopTransform.SetRotation(FQuat::MakeFromEuler(FVector(0.0f, -90.0f, 0.0f)));
    OutCameraTransforms.Add(TopTransform);

    FTransform BottomTransform;
    BottomTransform.SetLocation(ActorCenter + FVector(0.0f, 0.0f, -Distance));
    BottomTransform.SetRotation(FQuat::MakeFromEuler(FVector(0.0f, 90.0f, 0.0f)));
    OutCameraTransforms.Add(BottomTransform);*/
  }
  else if (CameraOptions.CameraMode == EComfyTexturesCameraMode::Orbit)
  {
    // get the actor bounds
    FBox ActorBounds = Actor->GetComponentsBoundingBox();

    // get the actor center
    FVector ActorCenter = ActorBounds.GetCenter();

    // get the actor extent
    FVector ActorExtent = ActorBounds.GetExtent();

    // create a transform for each orbit step
    // OrbitSteps - number of steps to split the orbit into
    // OrbitHeight - height of the orbit
    // make sure to look at the actor center

    float CameraFov = 90.0f;
    float GreatestExtent = FMath::Max(ActorExtent.X, FMath::Max(ActorExtent.Y, ActorExtent.Z));
    float Distance = GreatestExtent / FMath::Tan(CameraFov * (float)PI / 360.0f);

    for (int Index = 0; Index < CameraOptions.OrbitSteps; Index++)
    {
      float Angle = Index * 360.0f / CameraOptions.OrbitSteps;
      float X = FMath::Cos(Angle * (float)PI / 180.0f);
      float Y = FMath::Sin(Angle * (float)PI / 180.0f);
      float Z = CameraOptions.OrbitHeight;

      /*FTransform Transform;
      Transform.SetLocation(ActorCenter + FVector(X * Distance, Y * Distance, Z));
      Transform.SetRotation(FQuat::MakeFromEuler(FVector(0.0f, 0.0f, Angle)));
      OutCameraTransforms.Add(Transform);
      OutPromptAffixes.Add(FString::Printf(TEXT("orbit angle %d"), (int)Angle));*/
    }
  }
  else
  {
    UE_LOG(LogTemp, Error, TEXT("Unsupported camera mode."));
    return false;
  }

  return true;
}

bool UComfyTexturesWidgetBase::CaptureSceneTextures(UWorld* World, TArray<AActor*> Actors, const TArray<FMinimalViewInfo>& ViewInfos, EComfyTexturesMode Mode, TArray<FComfyTexturesCaptureOutput>& Outputs) const
{
  // create RTF RGBA8 render target
  UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>();
  RenderTarget->InitCustomFormat(1024, 1024, EPixelFormat::PF_FloatRGBA, true);
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
      UE_LOG(LogTemp, Error, TEXT("Failed to read render target pixels."));
      return false;
    }

    if (!ReadRenderTargetPixels(RenderTarget, EComfyTexturesRenderTextureMode::RawDepth, Output.RawDepth))
    {
      UE_LOG(LogTemp, Error, TEXT("Failed to read render target pixels."));
      return false;
    }

    SceneCapture->CaptureSource = ESceneCaptureSource::SCS_BaseColor;
    SceneCapture->CaptureScene();

    if (!ReadRenderTargetPixels(RenderTarget, EComfyTexturesRenderTextureMode::Color, Output.Color))
    {
      UE_LOG(LogTemp, Error, TEXT("Failed to read render target pixels."));
      return false;
    }

    Output.EditMask.Width = Output.Color.Width;
    Output.EditMask.Height = Output.Color.Height;
    CreateEditMaskFromImage(Output.Color.Pixels, Output.EditMask.Pixels);

    SceneCapture->CaptureSource = ESceneCaptureSource::SCS_Normal;
    SceneCapture->CaptureScene();

    if (!ReadRenderTargetPixels(RenderTarget, EComfyTexturesRenderTextureMode::Normals, Output.Normals))
    {
      UE_LOG(LogTemp, Error, TEXT("Failed to read render target pixels."));
      return false;
    }

    CreateEdgeMask(Output.Depth, Output.Normals, Output.EdgeMask);

    Outputs.Add(MoveTemp(Output));
  }

  // destroy the scene capture component
  SceneCapture->DestroyComponent();
  SceneCapture = nullptr;

  // destroy the render target
  RenderTarget->ConditionalBeginDestroy();
  RenderTarget = nullptr;

  return true;
}
