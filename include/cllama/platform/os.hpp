#pragma once

#if defined(_WIN32)
#  include <cllama/platform/windows/os.hpp>
#elif defined(__APPLE__)
#  include <TargetConditionals.h>
#  if TARGET_OS_IPHONE
#    include <cllama/platform/ios/os.hpp>
#  else
#    include <cllama/platform/macos/os.hpp>
#  endif
#elif defined(__ANDROID__)
#  include <cllama/platform/android/os.hpp>
#else
#  include <cllama/platform/linux/os.hpp>
#endif
