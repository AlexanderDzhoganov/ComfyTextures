// Fill out your copyright notice in the Description page of Project Settings.

#include "ComfyTexturesTests.h"
#include "PromptProcessor.h"
#include "MCPClient.h"

UComfyTexturesTests::UComfyTexturesTests()
    : TestsPassed(0)
    , TestsFailed(0)
{
}

void UComfyTexturesTests::RunAllTests()
{
    UE_LOG(LogComfyTextures, Log, TEXT("=== Starting ComfyTextures Cross-Platform Tests ==="));

    TestResults.Empty();
    TestsPassed = 0;
    TestsFailed = 0;

    // Run individual tests
    TestPromptProcessing();
    TestMCPClientSimulation();
    TestCrossPlatformValidation();
    TestTextureParameterGeneration();
    TestSceneDataSerialization();

    PrintTestSummary();

    UE_LOG(LogComfyTextures, Log, TEXT("=== ComfyTextures Tests Completed ==="));
}

bool UComfyTexturesTests::TestPromptProcessing()
{
    bool bAllPassed = true;

    bAllPassed &= TestPromptParsingBasic();
    bAllPassed &= TestPromptParsingComplex();
    bAllPassed &= TestPlatformOptimization();

    return bAllPassed;
}

bool UComfyTexturesTests::TestMCPClientSimulation()
{
    UMCPClient* TestClient = NewObject<UMCPClient>();
    TestClient->Initialize("127.0.0.1", 25565, "TestUser");

    // Test initialization
    bool bInitTest = TestClient->IsConnected() == false;
    LogTestResult("MCP Client Initialization", bInitTest);

    // Note: Actual connection test would require a running server
    // For now, we just test the interface

    return bInitTest;
}

bool UComfyTexturesTests::TestCrossPlatformValidation()
{
    bool bAllPassed = true;

    bAllPassed &= TestPromptValidationUnreal();
    bAllPassed &= TestPromptValidationMinecraft();

    return bAllPassed;
}

bool UComfyTexturesTests::TestTextureParameterGeneration()
{
    return TestParameterGeneration();
}

bool UComfyTexturesTests::TestSceneDataSerialization()
{
    // Test basic serialization (placeholder for now)
    TArray<uint8> TestData = { 1, 2, 3, 4, 5 };
    FString Encoded = FBase64::Encode(TestData);
    TArray<uint8> Decoded;
    FBase64::Decode(Encoded, Decoded);

    bool bSerializationTest = TestData == Decoded;
    LogTestResult("Scene Data Serialization", bSerializationTest);

    return bSerializationTest;
}

void UComfyTexturesTests::LogTestResult(const FString& TestName, bool bPassed, const FString& Message)
{
    FString Result = bPassed ? "PASSED" : "FAILED";
    FString LogMessage = FString::Printf(TEXT("Test %s: %s"), *TestName, *Result);

    if (!Message.IsEmpty())
    {
        LogMessage += FString::Printf(TEXT(" - %s"), *Message);
    }

    TestResults.Add(LogMessage);

    if (bPassed)
    {
        TestsPassed++;
        UE_LOG(LogComfyTextures, Log, TEXT("%s"), *LogMessage);
    }
    else
    {
        TestsFailed++;
        UE_LOG(LogComfyTextures, Error, TEXT("%s"), *LogMessage);
    }
}

void UComfyTexturesTests::PrintTestSummary()
{
    UE_LOG(LogComfyTextures, Log, TEXT("=== Test Summary ==="));
    UE_LOG(LogComfyTextures, Log, TEXT("Total Tests: %d"), TestResults.Num());
    UE_LOG(LogComfyTextures, Log, TEXT("Passed: %d"), TestsPassed);
    UE_LOG(LogComfyTextures, Log, TEXT("Failed: %d"), TestsFailed);

    if (TestsFailed > 0)
    {
        UE_LOG(LogComfyTextures, Log, TEXT("Failed Tests:"));
        for (const FString& Result : TestResults)
        {
            if (Result.Contains("FAILED"))
            {
                UE_LOG(LogComfyTextures, Log, TEXT("  %s"), *Result);
            }
        }
    }
}

bool UComfyTexturesTests::TestPromptParsingBasic()
{
    UPromptProcessor* Processor = NewObject<UPromptProcessor>();

    FPromptComponents Components;
    FString TestPrompt = "a beautiful forest with glowing mushrooms";

    bool bParseTest = Processor->ParsePrompt(TestPrompt, Components);
    bool bSubjectTest = Components.Subject.Contains("forest");
    bool bEnvironmentTest = Components.Environment.Contains("forest");

    LogTestResult("Basic Prompt Parsing", bParseTest && bSubjectTest && bEnvironmentTest,
                  FString::Printf(TEXT("Subject: %s, Environment: %s"), *Components.Subject, *Components.Environment));

    return bParseTest && bSubjectTest && bEnvironmentTest;
}

bool UComfyTexturesTests::TestPromptParsingComplex()
{
    UPromptProcessor* Processor = NewObject<UPromptProcessor>();

    FPromptComponents Components;
    FString TestPrompt = "high quality detailed mountain landscape at sunset with dramatic lighting";

    bool bParseTest = Processor->ParsePrompt(TestPrompt, Components);
    bool bQualityTest = Components.Quality.Contains("high");
    bool bLightingTest = Components.Lighting.Contains("sunset");

    LogTestResult("Complex Prompt Parsing", bParseTest && bQualityTest && bLightingTest);

    return bParseTest && bQualityTest && bLightingTest;
}

bool UComfyTexturesTests::TestPromptValidationUnreal()
{
    UPromptProcessor* Processor = NewObject<UPromptProcessor>();

    TArray<FString> ValidationErrors;
    FString TestPrompt = "4k ultra detailed realistic scene";

    bool bValidationTest = Processor->ValidatePromptForPlatform(TestPrompt, "Unreal", ValidationErrors);

    LogTestResult("Unreal Prompt Validation", bValidationTest,
                  ValidationErrors.Num() > 0 ? ValidationErrors[0] : "No errors");

    return bValidationTest;
}

bool UComfyTexturesTests::TestPromptValidationMinecraft()
{
    UPromptProcessor* Processor = NewObject<UPromptProcessor>();

    TArray<FString> ValidationErrors;
    FString TestPrompt = "volumetric fog effects in minecraft style";

    bool bValidationTest = Processor->ValidatePromptForPlatform(TestPrompt, "Minecraft", ValidationErrors);
    // This should produce validation errors for volumetric effects

    LogTestResult("Minecraft Prompt Validation", !bValidationTest && ValidationErrors.Num() > 0,
                  ValidationErrors.Num() > 0 ? ValidationErrors[0] : "Expected validation errors");

    return !bValidationTest && ValidationErrors.Num() > 0;
}

bool UComfyTexturesTests::TestParameterGeneration()
{
    UPromptProcessor* Processor = NewObject<UPromptProcessor>();

    FPromptComponents Components;
    Components.Subject = "forest";
    Components.Quality = "high";
    Components.TargetPlatform = "Unreal";

    FTextureGenerationParams Params;
    bool bGenerationTest = Processor->GenerateTextureParams(Components, Params);

    bool bParamsValid = Params.Resolution > 0 && Params.Steps > 0 && Params.GuidanceScale > 0;

    LogTestResult("Parameter Generation", bGenerationTest && bParamsValid,
                  FString::Printf(TEXT("Resolution: %d, Steps: %d"), Params.Resolution, Params.Steps));

    return bGenerationTest && bParamsValid;
}

bool UComfyTexturesTests::TestPlatformOptimization()
{
    UPromptProcessor* Processor = NewObject<UPromptProcessor>();

    FString OriginalPrompt = "realistic detailed forest";
    FString OptimizedUnreal = Processor->OptimizePromptForPlatform(OriginalPrompt, "Unreal");
    FString OptimizedMinecraft = Processor->OptimizePromptForPlatform(OriginalPrompt, "Minecraft");

    bool bUnrealOptimized = OptimizedUnreal.Contains("highly detailed");
    bool bMinecraftOptimized = OptimizedMinecraft.Contains("minecraft style");

    LogTestResult("Platform Optimization", bUnrealOptimized && bMinecraftOptimized);

    return bUnrealOptimized && bMinecraftOptimized;
}