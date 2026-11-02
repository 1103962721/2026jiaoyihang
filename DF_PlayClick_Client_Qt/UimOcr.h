#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

struct UimTextBlock
{
    std::string textUtf8;
    RECT bounds{};       // Bound-window client coordinates.
    POINT corners[4]{}; // OCR quadrilateral in the same coordinates.
};

struct UimType
{
    std::string textUtf8;
    std::vector<UimTextBlock> blocks;
    DWORD elapsedMs = 0;
    std::string errorUtf8;
};

// Uses [OCR] Port and UmiOcrPath from DF_PlayClick_Client.ini beside the host EXE.
// Starts Umi-OCR if necessary. Call from a worker thread if startup may block the UI.
BOOL OnUimOcr();

// Performs one OCR request. Coordinates are relative to hwnd's client area, without scaling.
// The returned UTF-8 pointer belongs to result and stays valid until result is modified or destroyed.
// Returns nullptr on failure; successful recognition with no text returns an empty string.
PCHAR UimOcrReadText(HWND hwnd, int pointX, int pointY, int width, int height, UimType* result);

// Left-clicks a point relative to hwnd's client area with real mouse input.
// Returns FALSE for an invalid window/point or when input cannot be sent.
BOOL HwdnClick(HWND hwnd, int x, int y);
