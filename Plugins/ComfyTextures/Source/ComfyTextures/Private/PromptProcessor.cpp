// Fill out your copyright notice in the Description page of Project Settings.

#include "PromptProcessor.h"

UPromptProcessor::UPromptProcessor()
{
}

bool UPromptProcessor::ParsePrompt(const FString& Prompt, FPromptComponents& OutComponents)
{
    if (Prompt.IsEmpty())
    {
        UE_LOG(LogComfyTextures, Error, TEXT("Empty prompt provided"));
        return false;
    }

    FString NormalizedPrompt = NormalizeText(Prompt);

    // Extract components
    ExtractSubject(NormalizedPrompt, OutComponents.Subject);
    ExtractEnvironment(NormalizedPrompt, OutComponents.Environment);
    ExtractLighting(NormalizedPrompt, OutComponents.Lighting);
    ExtractStyle(NormalizedPrompt, OutComponents.Style);
    ExtractQuality(NormalizedPrompt, OutComponents.Quality);
    ExtractPlatform(NormalizedPrompt, OutComponents.TargetPlatform);
    ExtractModifiers(NormalizedPrompt, OutComponents.Modifiers);

    // Validate that we have at least a subject
    if (OutComponents.Subject.IsEmpty())
    {
        UE_LOG(LogComfyTextures, Warning, TEXT("No subject found in prompt: %s"), *Prompt);
        OutComponents.Subject = "scene"; // Default fallback
    }

    return true;
}

bool UPromptProcessor::GenerateTextureParams(const FPromptComponents& Components, FTextureGenerationParams& OutParams)
{
    // Base parameters
    OutParams.Resolution = 1024;
    OutParams.Steps = 20;
    OutParams.GuidanceScale = 7.5f;
    OutParams.Seed = FMath::Rand();
    OutParams.bHighQuality = false;

    // Adjust based on quality
    MapQualityToParams(Components.Quality, OutParams);

    // Apply platform-specific adjustments
    ApplyPlatformSpecificParams(Components.TargetPlatform, OutParams);

    // Adjust based on style complexity
    if (!Components.Style.IsEmpty())
    {
        if (Components.Style.Contains("detailed") || Components.Style.Contains("complex"))
        {
            OutParams.Steps += 5;
            OutParams.Resolution = FMath::Max(OutParams.Resolution, 2048);
        }
    }

    return true;
}

bool UPromptProcessor::ValidatePromptForPlatform(const FString& Prompt, const FString& TargetPlatform, TArray<FString>& OutValidationErrors)
{
    OutValidationErrors.Empty();

    if (Prompt.IsEmpty())
    {
        OutValidationErrors.Add("Prompt cannot be empty");
        return false;
    }

    if (TargetPlatform.IsEmpty())
    {
        OutValidationErrors.Add("Target platform must be specified");
        return false;
    }

    // Check for platform-specific incompatibilities
    if (TargetPlatform == "Minecraft")
    {
        // Minecraft has limitations on certain visual effects
        if (Prompt.Contains("volumetric") || Prompt.Contains("fog"))
        {
            OutValidationErrors.Add("Volumetric effects and fog may not render well in Minecraft");
        }

        if (Prompt.Contains("high detail") || Prompt.Contains("4k"))
        {
            OutValidationErrors.Add("High resolution textures may impact Minecraft performance");
        }
    }
    else if (TargetPlatform == "Unreal")
    {
        // Unreal can handle more complex effects
        // Add any Unreal-specific validations here
    }

    return OutValidationErrors.Num() == 0;
}

FString UPromptProcessor::OptimizePromptForPlatform(const FString& Prompt, const FString& TargetPlatform)
{
    FString OptimizedPrompt = Prompt;

    if (TargetPlatform == "Minecraft")
    {
        // Optimize for Minecraft's block-based aesthetic
        OptimizedPrompt = OptimizedPrompt.Replace(TEXT("realistic"), TEXT("blocky"));
        OptimizedPrompt = OptimizedPrompt.Replace(TEXT("photorealistic"), TEXT("stylized"));
        OptimizedPrompt = OptimizedPrompt.Replace(TEXT("smooth"), TEXT("pixelated"));

        // Add Minecraft-specific terms if not present
        if (!OptimizedPrompt.Contains("minecraft") && !OptimizedPrompt.Contains("block"))
        {
            OptimizedPrompt += " in minecraft style";
        }
    }
    else if (TargetPlatform == "Unreal")
    {
        // Optimize for Unreal's high-fidelity rendering
        if (!OptimizedPrompt.Contains("detailed") && !OptimizedPrompt.Contains("high quality"))
        {
            OptimizedPrompt += " highly detailed";
        }
    }

    return OptimizedPrompt;
}

bool UPromptProcessor::ExtractSubject(const FString& Prompt, FString& OutSubject)
{
    // Simple subject extraction - look for nouns before prepositions
    TArray<FString> Words;
    Prompt.ParseIntoArray(Words, TEXT(" "), true);

    for (int32 i = 0; i < Words.Num(); ++i)
    {
        FString Word = Words[i].ToLower();

        // Skip articles and prepositions
        if (Word == "a" || Word == "an" || Word == "the" || Word == "in" || Word == "on" || Word == "at" || Word == "with")
        {
            continue;
        }

        // Look for common scene subjects
        if (Word.Contains("forest") || Word.Contains("mountain") || Word.Contains("ocean") ||
            Word.Contains("city") || Word.Contains("building") || Word.Contains("landscape") ||
            Word.Contains("character") || Word.Contains("creature"))
        {
            OutSubject = Words[i];
            return true;
        }
    }

    // Fallback: use first meaningful word
    if (Words.Num() > 0)
    {
        OutSubject = Words[0];
        return true;
    }

    return false;
}

bool UPromptProcessor::ExtractEnvironment(const FString& Prompt, FString& OutEnvironment)
{
    // Look for environment keywords
    if (Prompt.Contains("forest") || Prompt.Contains("woods"))
    {
        OutEnvironment = "forest";
    }
    else if (Prompt.Contains("mountain") || Prompt.Contains("mountains"))
    {
        OutEnvironment = "mountain";
    }
    else if (Prompt.Contains("ocean") || Prompt.Contains("sea"))
    {
        OutEnvironment = "ocean";
    }
    else if (Prompt.Contains("desert"))
    {
        OutEnvironment = "desert";
    }
    else if (Prompt.Contains("city") || Prompt.Contains("urban"))
    {
        OutEnvironment = "urban";
    }
    else
    {
        OutEnvironment = "generic";
    }

    return true;
}

bool UPromptProcessor::ExtractLighting(const FString& Prompt, FString& OutLighting)
{
    if (Prompt.Contains("sunset") || Prompt.Contains("dusk"))
    {
        OutLighting = "sunset";
    }
    else if (Prompt.Contains("sunrise") || Prompt.Contains("dawn"))
    {
        OutLighting = "sunrise";
    }
    else if (Prompt.Contains("noon") || Prompt.Contains("daylight"))
    {
        OutLighting = "daylight";
    }
    else if (Prompt.Contains("night") || Prompt.Contains("moonlight"))
    {
        OutLighting = "night";
    }
    else if (Prompt.Contains("storm") || Prompt.Contains("rain"))
    {
        OutLighting = "stormy";
    }
    else
    {
        OutLighting = "natural";
    }

    return true;
}

bool UPromptProcessor::ExtractStyle(const FString& Prompt, FString& OutStyle)
{
    if (Prompt.Contains("realistic") || Prompt.Contains("photorealistic"))
    {
        OutStyle = "realistic";
    }
    else if (Prompt.Contains("cartoon") || Prompt.Contains("animated"))
    {
        OutStyle = "cartoon";
    }
    else if (Prompt.Contains("abstract") || Prompt.Contains("modern"))
    {
        OutStyle = "abstract";
    }
    else if (Prompt.Contains("vintage") || Prompt.Contains("retro"))
    {
        OutStyle = "vintage";
    }
    else
    {
        OutStyle = "natural";
    }

    return true;
}

bool UPromptProcessor::ExtractQuality(const FString& Prompt, FString& OutQuality)
{
    if (Prompt.Contains("4k") || Prompt.Contains("ultra high") || Prompt.Contains("maximum quality"))
    {
        OutQuality = "ultra_high";
    }
    else if (Prompt.Contains("high quality") || Prompt.Contains("detailed"))
    {
        OutQuality = "high";
    }
    else if (Prompt.Contains("medium") || Prompt.Contains("standard"))
    {
        OutQuality = "medium";
    }
    else if (Prompt.Contains("low") || Prompt.Contains("fast"))
    {
        OutQuality = "low";
    }
    else
    {
        OutQuality = "medium"; // Default
    }

    return true;
}

bool UPromptProcessor::ExtractPlatform(const FString& Prompt, FString& OutPlatform)
{
    if (Prompt.Contains("minecraft") || Prompt.Contains("block") || Prompt.Contains("pixel"))
    {
        OutPlatform = "Minecraft";
    }
    else if (Prompt.Contains("unreal") || Prompt.Contains("high fidelity") || Prompt.Contains("realistic"))
    {
        OutPlatform = "Unreal";
    }
    else
    {
        OutPlatform = "Unreal"; // Default to Unreal for higher quality
    }

    return true;
}

void UPromptProcessor::ExtractModifiers(const FString& Prompt, TArray<FString>& OutModifiers)
{
    // Extract additional descriptive elements
    TArray<FString> Words;
    Prompt.ParseIntoArray(Words, TEXT(" "), true);

    for (const FString& Word : Words)
    {
        FString LowerWord = Word.ToLower();

        // Look for adjectives and adverbs that modify the scene
        if (LowerWord.Contains("glowing") || LowerWord.Contains("shiny") ||
            LowerWord.Contains("dark") || LowerWord.Contains("bright") ||
            LowerWord.Contains("colorful") || LowerWord.Contains("monochrome"))
        {
            OutModifiers.Add(Word);
        }
    }
}

FString UPromptProcessor::NormalizeText(const FString& Text)
{
    FString Normalized = Text.ToLower();
    // Remove extra whitespace and punctuation
    Normalized = Normalized.Replace(TEXT(","), TEXT(""));
    Normalized = Normalized.Replace(TEXT("."), TEXT(""));
    Normalized = Normalized.Replace(TEXT("!"), TEXT(""));
    Normalized = Normalized.Replace(TEXT("?"), TEXT(""));

    return Normalized;
}

bool UPromptProcessor::ContainsPlatformKeywords(const FString& Text, const FString& Platform)
{
    FString LowerText = Text.ToLower();

    if (Platform == "Minecraft")
    {
        return LowerText.Contains("minecraft") || LowerText.Contains("block") ||
               LowerText.Contains("pixel") || LowerText.Contains("8-bit");
    }
    else if (Platform == "Unreal")
    {
        return LowerText.Contains("unreal") || LowerText.Contains("high fidelity") ||
               LowerText.Contains("realistic") || LowerText.Contains("detailed");
    }

    return false;
}

void UPromptProcessor::MapQualityToParams(const FString& Quality, FTextureGenerationParams& OutParams)
{
    if (Quality == "ultra_high")
    {
        OutParams.Resolution = 2048;
        OutParams.Steps = 50;
        OutParams.GuidanceScale = 12.0f;
        OutParams.bHighQuality = true;
    }
    else if (Quality == "high")
    {
        OutParams.Resolution = 1024;
        OutParams.Steps = 30;
        OutParams.GuidanceScale = 8.0f;
        OutParams.bHighQuality = true;
    }
    else if (Quality == "medium")
    {
        OutParams.Resolution = 512;
        OutParams.Steps = 20;
        OutParams.GuidanceScale = 7.5f;
        OutParams.bHighQuality = false;
    }
    else if (Quality == "low")
    {
        OutParams.Resolution = 256;
        OutParams.Steps = 10;
        OutParams.GuidanceScale = 6.0f;
        OutParams.bHighQuality = false;
    }
}

void UPromptProcessor::ApplyPlatformSpecificParams(const FString& Platform, FTextureGenerationParams& OutParams)
{
    if (Platform == "Minecraft")
    {
        // Minecraft optimized settings
        OutParams.Resolution = FMath::Min(OutParams.Resolution, 512); // Limit resolution for performance
        OutParams.PlatformParams.Add("texture_format", "block_texture");
        OutParams.PlatformParams.Add("color_palette", "minecraft");
    }
    else if (Platform == "Unreal")
    {
        // Unreal optimized settings
        OutParams.PlatformParams.Add("texture_format", "high_fidelity");
        OutParams.PlatformParams.Add("normal_map", "true");
        OutParams.PlatformParams.Add("roughness_map", "true");
    }
}