add_library(vc_core STATIC
    src/FeedbackSuppressor.cpp
    src/SpectralDenoiser.cpp
    src/AsyncMaskProvider.cpp
    src/AutoMixer.cpp
    src/BypassMixer.cpp
    src/FeedbackCanceller.cpp)
target_include_directories(vc_core PUBLIC include)
target_compile_features(vc_core PUBLIC cxx_std_17)
find_package(Threads REQUIRED)
target_link_libraries(vc_core PUBLIC Threads::Threads)

if(APPLE)
    enable_language(OBJCXX)
    target_sources(vc_core PRIVATE src/CoreMlMaskProvider.mm)
    target_compile_options(vc_core PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(vc_core PUBLIC "-framework CoreML" "-framework Foundation")
else()
    target_sources(vc_core PRIVATE src/CoreMlMaskProvider_stub.cpp)
endif()

if(NOT MSVC)
    target_compile_options(vc_core PRIVATE -Wall -Wextra)
endif()
