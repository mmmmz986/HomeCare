cmake_minimum_required(VERSION 3.20)
project(recognize_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt 자동 처리
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Sql SerialPort)
find_package(OpenCV REQUIRED)

set(TARGET recognize_app)

add_executable(${TARGET}
    main.cpp
    MainWindow.cpp
    MainWindow.h
    MainWindow.ui
    DbManager.h
    ConfigLoader.h
)

# (옵션) opencv_contrib face 쓰고 싶을 때만 ON
option(USE_OPENCV_FACE "Use OpenCV contrib face module" OFF)
if (USE_OPENCV_FACE)
    target_compile_definitions(${TARGET} PRIVATE HAS_OPENCV_FACE)
endif()

target_link_libraries(${TARGET}
    PRIVATE Qt6::Widgets Qt6::Sql Qt6::SerialPort ${OpenCV_LIBS}
)
