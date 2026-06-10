#pragma once
#include <string>
#include <map>

namespace S3MM {

// ASSEMBLY LOAD HOOKS - Track assembly loads and detect conflicts via mono_image_open hooks

// Install file-based and memory-based hooks, captures .package mods loaded from memory
bool InstallImageLoadHooks();

// Get the original load path for a MonoImage, empty if not tracked
std::string GetImageLoadPath(void* image);

// Get all tracked assembly-to-package mappings
std::map<std::string, std::string> GetAllAssemblyPackageSources();

// LOAD TRACKING - Count of assembly images opened since hooks were installed
int GetImageLoadCount();

} // namespace S3MM
