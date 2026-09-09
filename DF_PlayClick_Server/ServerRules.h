#pragma once

#include <cerrno>
#include <climits>
#include <cwchar>

inline int ParseCountdownSeconds(const wchar_t* text)
{
    if (!text)
    {
        return -1;
    }

    errno = 0;
    wchar_t* end = nullptr;
    long value = std::wcstol(text, &end, 10);
    if (end == text || errno == ERANGE || value < 0 || value > INT_MAX)
    {
        return -1;
    }

    while (*end == L' ' || *end == L'\t')
    {
        ++end;
    }

    return *end == L'\0' ? static_cast<int>(value) : -1;
}
