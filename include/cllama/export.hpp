#pragma once

#if defined(_WIN32) && (defined(BUILD_SHARED_LIBS) || defined(CLLAMA_EXPORTS))
#  ifdef CLLAMA_EXPORTS
#    define CLLAMA_API __declspec(dllexport)
#  else
#    define CLLAMA_API __declspec(dllimport)
#  endif
#else
#  define CLLAMA_API
#endif
