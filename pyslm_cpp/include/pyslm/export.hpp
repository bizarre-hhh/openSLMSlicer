#pragma once

#if defined(_WIN32)
#  if defined(PYSLM_CPP_BUILD)
#    define PYSLM_API __declspec(dllexport)
#  else
#    define PYSLM_API __declspec(dllimport)
#  endif
#else
#  define PYSLM_API __attribute__((visibility("default")))
#endif
