# Platform-specific sources — only one is compiled per target platform
set(PLATFORM_SRC "")

if(WIN32)
    set(PLATFORM_SRC "windows.cpp")
elseif(APPLE)
    if(IOS)
        set(PLATFORM_SRC "ios.cpp")
    else()
        set(PLATFORM_SRC "macos.cpp")
    endif()
elseif(ANDROID)
    set(PLATFORM_SRC "android.cpp")
else()
    set(PLATFORM_SRC "linux.cpp")
endif()

# Return the source to the parent scope
set(CLLAMA_PLATFORM_SRC ${CMAKE_CURRENT_SOURCE_DIR}/${PLATFORM_SRC} PARENT_SCOPE)
