#pragma once
#include <stdint.h>
#include <string>
#include <array>

#if defined(UNITY_WIN) || defined(UNITY_ANDROID) || defined(UNITY_IOS) || defined(UNITY_IPHONE) || defined(UNITY_MAC)
#include "UnityPrefix.h"
extern void LoadDbgScp(const core::string& log_path, const core::string& load_path);
#elif (defined(UE_BUILD_DEBUG) || defined(UE_BUILD_DEVELOPMENT) || defined(UE_BUILD_TEST) || defined(UE_BUILD_SHIPPING)) && defined(UE_SERVER) && !UE_SERVER
#include "CoreMinimal.h"
extern void LoadDbgScp(const FString& log_path, const FString& load_path);
#elif defined(DBGSCP_ON_MYUZU)
extern void LoadDbgScp(const std::string& log_path, const std::string& load_path);
#endif

extern void PauseDbgScp();
extern void ResumeDbgScp();

extern void DbgScp_Set_Extern(int cmd, int a, double b, const char* c);
extern int DbgScp_Get_Extern(int cmd, int a, double b, const char* c);
