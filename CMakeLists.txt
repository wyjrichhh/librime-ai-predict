# librime-ai-predict: CTranslate2-based prediction plugin for Rime.
# Build as a subdirectory of librime: place this repo under librime/plugins/
# and configure librime with the plugin present (or set RIME_PLUGINS).
cmake_minimum_required(VERSION 3.14)
project(rime-ai-predict LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(plugin_library rime-ai-predict)
set(ai_predict_module_name "ai_predict")

if(NOT CTRANSLATE2_ROOT AND DEFINED ENV{CTRANSLATE2_ROOT})
  set(CTRANSLATE2_ROOT "$ENV{CTRANSLATE2_ROOT}")
endif()
if(NOT CTRANSLATE2_ROOT)
  # Default: CT2 installed by `make deps` into this repo's include/ and lib/
  set(CTRANSLATE2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
set(CTRANSLATE2_ROOT "${CTRANSLATE2_ROOT}" CACHE PATH
  "CTranslate2 install prefix (run `make deps` in this directory first)")

find_path(CTRANSLATE2_INCLUDE_DIR NAMES ctranslate2/translator.h
  PATHS
    "${CTRANSLATE2_ROOT}/include"
    /usr/local/include
    /opt/homebrew/include
)
find_library(CTRANSLATE2_LIBRARY NAMES ctranslate2
  PATHS
    "${CTRANSLATE2_ROOT}/lib"
    /usr/local/lib
    /opt/homebrew/lib
)
if(NOT CTRANSLATE2_INCLUDE_DIR OR NOT CTRANSLATE2_LIBRARY)
  message(FATAL_ERROR
    "CTranslate2 not found. From this plugin directory run: make deps\n"
    "Or set -DCTRANSLATE2_ROOT to an install prefix containing ctranslate2.")
endif()

set(CTRANSLATE2_LIBRARIES ${CTRANSLATE2_LIBRARY})
find_library(CPU_FEATURES_LIBRARY NAMES cpu_features
  PATHS "${CTRANSLATE2_ROOT}/lib" NO_DEFAULT_PATH
)
if(CPU_FEATURES_LIBRARY)
  list(APPEND CTRANSLATE2_LIBRARIES ${CPU_FEATURES_LIBRARY})
endif()
if(APPLE)
  find_library(ACCELERATE_FRAMEWORK Accelerate REQUIRED)
  list(APPEND CTRANSLATE2_LIBRARIES ${ACCELERATE_FRAMEWORK})
endif()
list(APPEND CTRANSLATE2_LIBRARIES ${CMAKE_DL_LIBS})

set(RIME_AI_PREDICT_SRC
  src/predict_module.cc
  src/predict_translator.cc
  src/predict_filter.cc
  src/prediction_engine.cc
  src/context_builder.cc
  src/ct2_backend.cc
)

add_library(rime-ai-predict-objs OBJECT ${RIME_AI_PREDICT_SRC})
if(BUILD_SHARED_LIBS)
  set_target_properties(rime-ai-predict-objs PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
target_include_directories(rime-ai-predict-objs PRIVATE
  ${CTRANSLATE2_INCLUDE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(rime-ai-predict-objs PRIVATE ${CTRANSLATE2_LIBRARIES})

# glog 0.7+ headers require these macros (same idea as librime/CMakeLists.txt).
if(WIN32)
  target_compile_definitions(rime-ai-predict-objs PRIVATE
    "GLOG_DEPRECATED=__declspec(deprecated)"
    GLOG_NO_ABBREVIATED_SEVERITIES)
else()
  target_compile_definitions(rime-ai-predict-objs PRIVATE
    "GLOG_DEPRECATED=__attribute__((deprecated))"
    "GLOG_EXPORT="
    "GLOG_NO_EXPORT=")
endif()

set(plugin_name ${plugin_library} PARENT_SCOPE)
set(plugin_objs $<TARGET_OBJECTS:rime-ai-predict-objs> PARENT_SCOPE)
set(plugin_deps ${rime_library} ${CTRANSLATE2_LIBRARIES} PARENT_SCOPE)
set(plugin_modules ${ai_predict_module_name} PARENT_SCOPE)

message(STATUS "${plugin_library} provides modules: ${ai_predict_module_name}")
message(STATUS "  CTranslate2: ${CTRANSLATE2_INCLUDE_DIR} -> ${CTRANSLATE2_LIBRARIES}")
