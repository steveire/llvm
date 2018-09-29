
# build libreplxx
add_llvm_library(
  replxx
  src/conversion.cxx
  src/ConvertUTF.cpp
  src/escape.cxx
  src/history.cxx
  src/inputbuffer.cxx
  src/io.cxx
  src/prompt.cxx
  src/replxx.cxx
  src/util.cxx
  src/wcwidth.cpp
  src/windows.cxx
)

add_library(Replxx::Replxx ALIAS replxx)

target_include_directories(replxx
   PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
   PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
