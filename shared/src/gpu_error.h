#pragma once
#include <Windows.h>

// Supplied by the owning renderer or GPU fixture. The UI backend does not
// depend on a particular application's Renderer class.
void hrCheck(HRESULT result, const char *operation);
