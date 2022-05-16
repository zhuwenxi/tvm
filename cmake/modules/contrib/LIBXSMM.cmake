if(IS_DIRECTORY ${USE_LIBXSMM})
  find_library(LIBXSMM_LIBRARY NAMES xsmm HINTS ${USE_LIBXSMM}/lib/)
  if(LIBXSMM_LIBRARY STREQUAL "LIBXSMM_LIBRARY-NOTFOUND")
    message(WARNING "Cannot find LIBXSMM library at ${USE_LIBXSMM}/lib/.")
  else()
    include_directories(SYSTEM ${USE_LIBXSMM}/include)
    list(APPEND TVM_RUNTIME_LINKER_LIBS ${LIBXSMM_LIBRARY})
    MESSAGE(STATUS "Use LIBXSMM library " ${LIBXSMM_LIBRARY})

    tvm_file_glob(GLOB LIBXSMM_RELAY_CONTRIB_SRC src/relay/backend/contrib/libxsmm/*.cc)
    list(APPEND COMPILER_SRCS ${LIBXSMM_RELAY_CONTRIB_SRC})
    tvm_file_glob(GLOB LIBXSMM_RUNTIME_SRC src/runtime/contrib/libxsmm/*.cc)
    list(APPEND RUNTIME_SRCS ${LIBXSMM_RUNTIME_SRC})
  endif()
elseif(USE_LIBXSMM STREQUAL "ON")
  find_library(LIBXSMM_LIBRARY xsmm)
  if(LIBXSMM_LIBRARY STREQUAL "LIBXSMM_LIBRARY-NOTFOUND")
    message(WARNING "Cannot find LIBXSMM library at $(USE_LIBXSMM).")
  else()
    list(APPEND TVM_RUNTIME_LINKER_LIBS ${LIBXSMM_LIBRARY})
    MESSAGE(STATUS "Use LIBXSMM library " ${LIBXSMM_LIBRARY})

    tvm_file_glob(GLOB LIBXSMM_RELAY_CONTRIB_SRC src/relay/backend/contrib/libxsmm/*.cc)
    list(APPEND COMPILER_SRCS ${LIBXSMM_RELAY_CONTRIB_SRC})
    tvm_file_glob(GLOB LIBXSMM_RUNTIME_SRC src/runtime/contrib/libxsmm/*.cc)
    list(APPEND RUNTIME_SRCS ${LIBXSMM_RUNTIME_SRC})
  endif()
elseif(USE_LIBXSMM STREQUAL "OFF")
else()
  message(FATAL_ERROR "Invalid option: USE_LIBXSMM=" ${USE_LIBXSMM})
endif()
