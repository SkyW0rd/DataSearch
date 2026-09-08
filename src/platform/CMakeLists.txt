if(WIN32)
  add_library(datasearch_platform STATIC
    win32/PlatformServiceWin32.cpp
  )
  target_link_libraries(datasearch_platform PUBLIC datasearch_core shell32 ole32)
else()
  # No implementation on this OS for stage 1 (ТЗ п.9/12.1: Windows-only deliverable).
  # Interface-only target so code that merely depends on IPlatformService's
  # declarations still compiles here; createPlatformService() has no definition
  # outside the Windows translation unit, so linking an app against this on
  # macOS/Linux would fail — which is intended, since no such app is built here.
  add_library(datasearch_platform INTERFACE)
endif()

if(WIN32)
  target_include_directories(datasearch_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
else()
  target_include_directories(datasearch_platform INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
endif()
