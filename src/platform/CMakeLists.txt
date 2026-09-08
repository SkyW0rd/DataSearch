if(WIN32)
  add_library(datasearch_platform STATIC
    win32/PlatformServiceWin32.cpp
  )
  target_link_libraries(datasearch_platform PUBLIC datasearch_core shell32 ole32)
elseif(APPLE)
  enable_language(OBJCXX)
  add_library(datasearch_platform STATIC
    macos/PlatformServiceMacOS.mm
  )
  set_source_files_properties(macos/PlatformServiceMacOS.mm PROPERTIES
    COMPILE_FLAGS "-fobjc-arc"
  )
  target_link_libraries(datasearch_platform PUBLIC datasearch_core
    "-framework Cocoa" "-framework CoreServices"
  )
elseif(UNIX)
  add_library(datasearch_platform STATIC
    linux/PlatformServiceLinux.cpp
  )
  target_link_libraries(datasearch_platform PUBLIC datasearch_core)
else()
  # No implementation for this OS yet. Interface-only target so code that
  # merely depends on IPlatformService's declarations still compiles here;
  # createPlatformService() has no definition, so linking an app against this
  # would fail — which is intended, since no such app is built here.
  add_library(datasearch_platform INTERFACE)
endif()

if(WIN32 OR APPLE OR UNIX)
  target_include_directories(datasearch_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
else()
  target_include_directories(datasearch_platform INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
endif()
