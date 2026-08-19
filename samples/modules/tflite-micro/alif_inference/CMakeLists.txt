cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alif_inference)

target_sources(app PRIVATE
    src/mfcc/PlatformMath.cc
    src/mfcc/KwsProcessing.cc
    src/mfcc/Mfcc.cc
    src/kws_micronet_m_vela_H128.tflite.cc
    src/main.cpp
    src/KWSModel.cpp
    src/LiveMicInput.cpp
)
