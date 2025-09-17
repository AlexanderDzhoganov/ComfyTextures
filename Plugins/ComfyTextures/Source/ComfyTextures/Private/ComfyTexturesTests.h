// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "ComfyTexturesTests.generated.h"

/**
 * Test framework for ComfyTextures cross-platform functionality
 */
UCLASS()
class COMFYTEXTURES_API UComfyTexturesTests : public UObject
{
    GENERATED_BODY()

public:
    UComfyTexturesTests();

    /** Run all tests */
    void RunAllTests();

    /** Test prompt processing */
    bool TestPromptProcessing();

    /** Test MCP client connection simulation */
    bool TestMCPClientSimulation();

    /** Test cross-platform validation */
    bool TestCrossPlatformValidation();

    /** Test texture parameter generation */
    bool TestTextureParameterGeneration();

    /** Test scene data serialization */
    bool TestSceneDataSerialization();

private:
    /** Test results */
    TArray<FString> TestResults;
    int32 TestsPassed;
    int32 TestsFailed;

    /** Helper methods */
    void LogTestResult(const FString& TestName, bool bPassed, const FString& Message = "");
    void PrintTestSummary();

    /** Individual test implementations */
    bool TestPromptParsingBasic();
    bool TestPromptParsingComplex();
    bool TestPromptValidationUnreal();
    bool TestPromptValidationMinecraft();
    bool TestParameterGeneration();
    bool TestPlatformOptimization();
};