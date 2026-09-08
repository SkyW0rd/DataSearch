set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(datasearch_app WIN32
  main.cpp
  MainWindow.h
  MainWindow.cpp
  ResultsTableModel.h
  ResultsTableModel.cpp
  IndexManager.h
  IndexManager.cpp
)

target_link_libraries(datasearch_app PRIVATE
  Qt6::Widgets
  datasearch_core
  datasearch_platform
)

if(APPLE)
  # A real .app bundle (not a bare executable) so macdeployqt can bundle Qt's
  # frameworks into it for standalone distribution (see .github/workflows).
  set_target_properties(datasearch_app PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "DataSearch"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.datasearch.app"
  )
endif()
