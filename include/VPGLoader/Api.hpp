#pragma once

#if defined(_WIN32)
#    if defined(VPGLOADER_STATIC)
#        define VPGLOADER_API
#    elif defined(VPGLOADER_BUILDING_LIBRARY)
#        define VPGLOADER_API __declspec(dllexport)
#    else
#        define VPGLOADER_API __declspec(dllimport)
#    endif
#elif defined(__GNUC__) || defined(__clang__)
#    define VPGLOADER_API __attribute__((visibility("default")))
#else
#    define VPGLOADER_API
#endif
