# TODO: Copyrights

IF (NOT EXISTS "${CMAKE_SOURCE_DIR}/storage/rocksdb/rocksdb/Makefile")
  MESSAGE(SEND_ERROR "Missing Makefile in rocksdb directory. Try \"git submodule update\".")
ENDIF()

SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

# get a list of rocksdb library source files
# run with env -i to avoid passing variables
EXECUTE_PROCESS(
  COMMAND env -i ${CMAKE_SOURCE_DIR}/storage/rocksdb/get_rocksdb_files.sh
  OUTPUT_VARIABLE SCRIPT_OUTPUT
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
# split the list into lines
STRING(REGEX MATCHALL "[^\n]+" ROCKSDB_LIB_SOURCES ${SCRIPT_OUTPUT})

INCLUDE_DIRECTORIES(
  ${CMAKE_SOURCE_DIR}/storage/rocksdb/rocksdb
  ${CMAKE_SOURCE_DIR}/storage/rocksdb/rocksdb/include
  ${CMAKE_SOURCE_DIR}/rocksdb/third-party/gtest-1.7.0/fused-src
)

ADD_DEFINITIONS(-DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX -DOS_LINUX
  -DZLIB)

# MARIAROCKS_NOT_YET: Add -frtti flag when compiling RocksDB files.
# TODO: is this the right way to do this?
#  - SQL layer and storage/rocksdb/*.cc are compiled with -fnortti
#  - RocksDB files are compiled with "-fnortti ... -frtti" 
#  - This causes RocksDB headers to be compiled with different settings:
#     = with RTTI when compiling RocksDB
#     = without RTTI when compiling storage/rocksdb/*.cc
# 
#  (facebook/mysql-5.6 just compiles everything without -f*rtti, which means
#  everything is compiled with -frtti) 
#
set_source_files_properties(${ROCKSDB_LIB_SOURCES} PROPERTIES COMPILE_FLAGS -frtti)

SET(ROCKSDB_SOURCES
  ha_rocksdb.cc ha_rocksdb.h ha_rocksdb_proto.h
  logger.h
  rdb_comparator.h
  rdb_datadic.cc rdb_datadic.h
  rdb_cf_options.cc rdb_cf_options.h
  rdb_cf_manager.cc rdb_cf_manager.h
  properties_collector.cc properties_collector.h
  event_listener.cc event_listener.h
  rdb_i_s.cc rdb_i_s.h
  rdb_index_merge.cc rdb_index_merge.h
  rdb_perf_context.cc rdb_perf_context.h
  rdb_mutex_wrapper.cc rdb_mutex_wrapper.h
  rdb_sst_info.cc rdb_sst_info.h
  rdb_utils.cc rdb_utils.h rdb_buff.h
  rdb_threads.cc rdb_threads.h
  rdb_mariadb_port.h
  ${ROCKSDB_LIB_SOURCES}
)

IF(WITH_FB_TSAN)
  SET(PIC_EXT "_pic")
ELSE()
  SET(PIC_EXT "")
ENDIF()

SET(rocksdb_static_libs )
IF (NOT "$ENV{WITH_SNAPPY}" STREQUAL "")
  SET(rocksdb_static_libs ${rocksdb_static_libs}
  $ENV{WITH_SNAPPY}/lib/libsnappy${PIC_EXT}.a)
  ADD_DEFINITIONS(-DSNAPPY)
ELSE()
  SET(rocksdb_static_libs ${rocksdb_static_libs} snappy)
ENDIF()

IF (NOT "$ENV{WITH_LZ4}" STREQUAL "")
  SET(rocksdb_static_libs ${rocksdb_static_libs}
  $ENV{WITH_LZ4}/lib/liblz4${PIC_EXT}.a)
  ADD_DEFINITIONS(-DLZ4)
ELSE()
  SET(rocksdb_static_libs ${rocksdb_static_libs} lz4)
ENDIF()

IF (NOT "$ENV{WITH_BZ2}" STREQUAL "")
  SET(rocksdb_static_libs ${rocksdb_static_libs}
  $ENV{WITH_BZ2}/lib/libbz2${PIC_EXT}.a)
  ADD_DEFINITIONS(-DBZIP2)
ELSE()
  SET(rocksdb_static_libs ${rocksdb_static_libs} bz2)
ENDIF()

# link ZSTD only if instructed
IF (NOT "$ENV{WITH_ZSTD}" STREQUAL "")
  SET(rocksdb_static_libs ${rocksdb_static_libs}
  $ENV{WITH_ZSTD}/lib/libzstd${PIC_EXT}.a)
  ADD_DEFINITIONS(-DZSTD)
ENDIF()

SET(rocksdb_static_libs ${rocksdb_static_libs} ${ZLIB_LIBRARY} "-lrt")

MYSQL_ADD_PLUGIN(rocksdb_se ${ROCKSDB_SOURCES} STORAGE_ENGINE DEFAULT STATIC_ONLY
  LINK_LIBRARIES ${rocksdb_static_libs}
)

IF(WITH_EMBEDDED_SERVER)
  ADD_SUBDIRECTORY(unittest)
ENDIF()

IF (WITH_ROCKSDB_SE_STORAGE_ENGINE)
  # TODO: read this file list from src.mk:TOOL_SOURCES
  SET(ROCKSDB_TOOL_SOURCES
    ${CMAKE_SOURCE_DIR}/storage/rocksdb/rocksdb/tools/ldb_tool.cc
    ${CMAKE_SOURCE_DIR}/storage/rocksdb/rocksdb/tools/ldb_cmd.cc
    ${CMAKE_SOURCE_DIR}/storage/rocksdb/rocksdb/tools/sst_dump_tool.cc
  )
  MYSQL_ADD_EXECUTABLE(sst_dump ${CMAKE_SOURCE_DIR}/storage/rocksdb/rocksdb/tools/sst_dump.cc ${ROCKSDB_TOOL_SOURCES})
  TARGET_LINK_LIBRARIES(sst_dump rocksdb_se)

  MYSQL_ADD_EXECUTABLE(ldb ${CMAKE_SOURCE_DIR}/storage/rocksdb/rocksdb/tools/ldb.cc ${ROCKSDB_TOOL_SOURCES})
  TARGET_LINK_LIBRARIES(ldb rocksdb_se)

  MYSQL_ADD_EXECUTABLE(mysql_ldb ${CMAKE_SOURCE_DIR}/storage/rocksdb/tools/mysql_ldb.cc ${ROCKSDB_TOOL_SOURCES})
  TARGET_LINK_LIBRARIES(mysql_ldb rocksdb_se)
ENDIF()