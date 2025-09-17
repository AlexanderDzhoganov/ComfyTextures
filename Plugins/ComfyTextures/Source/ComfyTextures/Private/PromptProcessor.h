// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "PromptProcessor.generated.h"

/**
 * Structure representing parsed prompt components
 */
USTRUCT(BlueprintType)
struct FPromptComponents
{
    GENERATED_BODY()

    /** Main subject of the scene */
    UPROPERTY(BlueprintReadOnly)
    FString Subject;

    /** Environment/setting description */
    UPROPERTY(BlueprintReadOnly)
    FString Environment;

    /** Lighting conditions */
    UPROPERTY(BlueprintReadOnly)
    FString Lighting;

    /** Style/artistic direction */
    UPROPERTY(BlueprintReadOnly)
    FString Style;

    /** Additional modifiers */
    UPROPERTY(BlueprintReadOnly)
    TArray<FString> Modifiers;

    /** Quality settings */
    UPROPERTY(BlueprintReadOnly)
    FString Quality;

    /** Target platform (Unreal/Minecraft) */
    UPROPERTY(BlueprintReadOnly)
    FString TargetPlatform;
};

/**
 * Structure for texture generation parameters
 */
USTRUCT(BlueprintType)
struct FTextureGenerationParams
{
    GENERATED_BODY()

    /** Resolution for texture generation */
    UPROPERTY(BlueprintReadOnly)
    int32 Resolution;

    /** Number of inference steps */
    UPROPERTY(BlueprintReadOnly)
    int32 Steps;

    /** Guidance scale */
    UPROPERTY(BlueprintReadOnly)
    float GuidanceScale;

    /** Seed for reproducible generation */
    UPROPERTY(BlueprintReadOnly)
    int32 Seed;

    /** Whether to use high quality mode */
    UPROPERTY(BlueprintReadOnly)
    bool bHighQuality;

    /** Platform-specific parameters */
    UPROPERTY(BlueprintReadOnly)
    TMap<FString, FString> PlatformParams;
};

/**
 * Prompt Processing Engine for cross-platform scene generation
 * Parses natural language prompts and converts them to structured parameters
 */
UCLASS()
class COMFYTEXTURES_API UPromptProcessor : public UObject
{
    GENERATED_BODY()

public:
    UPromptProcessor();

    /**
     * Parse a natural language prompt into structured components
     * @param Prompt The input prompt text
     * @param OutComponents Parsed components
     * @return True if parsing was successful
     */
    bool ParsePrompt(const FString& Prompt, FPromptComponents& OutComponents);

    /**
     * Convert parsed components to texture generation parameters
     * @param Components Parsed prompt components
     * @param OutParams Generated parameters
     * @return True if conversion was successful
     */
    bool GenerateTextureParams(const FPromptComponents& Components, FTextureGenerationParams& OutParams);

    /**
     * Validate prompt for cross-platform compatibility
     * @param Prompt The input prompt
     * @param TargetPlatform Target platform (Unreal/Minecraft)
     * @param OutValidationErrors List of validation errors
     * @return True if prompt is valid for the target platform
     */
    bool ValidatePromptForPlatform(const FString& Prompt, const FString& TargetPlatform, TArray<FString>& OutValidationErrors);

    /**
     * Optimize prompt for specific platform
     * @param Prompt Original prompt
     * @param TargetPlatform Target platform
     * @return Optimized prompt
     */
    FString OptimizePromptForPlatform(const FString& Prompt, const FString& TargetPlatform);

private:
    /** Parse subject from prompt */
    bool ExtractSubject(const FString& Prompt, FString& OutSubject);

    /** Parse environment description */
    bool ExtractEnvironment(const FString& Prompt, FString& OutEnvironment);

    /** Parse lighting conditions */
    bool ExtractLighting(const FString& Prompt, FString& OutLighting);

    /** Parse artistic style */
    bool ExtractStyle(const FString& Prompt, FString& OutStyle);

    /** Parse quality settings */
    bool ExtractQuality(const FString& Prompt, FString& OutQuality);

    /** Parse platform-specific elements */
    bool ExtractPlatform(const FString& Prompt, FString& OutPlatform);

    /** Extract modifiers and additional elements */
    void ExtractModifiers(const FString& Prompt, TArray<FString>& OutModifiers);

    /** Clean and normalize text */
    FString NormalizeText(const FString& Text);

    /** Check for platform-specific keywords */
    bool ContainsPlatformKeywords(const FString& Text, const FString& Platform);

    /** Map quality keywords to parameters */
    void MapQualityToParams(const FString& Quality, FTextureGenerationParams& OutParams);

    /** Platform-specific parameter mapping */
    void ApplyPlatformSpecificParams(const FString& Platform, FTextureGenerationParams& OutParams);
};