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
