# ISA flags for the targets that compile the SIMD distance kernels.
#
# The kernel test selects kernels with #ifdef __AVX2__ / __ARM_NEON, so a test
# built without the flag does not fail to link -- it silently compiles the
# branch away and reports success for a kernel it never called. The library
# must not take the flag wholesale: on x86_64 the global architecture flag is a
# baseline (-march=x86-64) in a portable build, and -mavx2 on every library
# translation unit would let the compiler emit AVX2 anywhere, ahead of the
# runtime CPU check. nvecd_add_simd_kernels() therefore confines it to the one
# source file that holds the AVX2 kernels.

include(CheckCXXCompilerFlag)

function(nvecd_target_simd_flags target)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86)|(X86)|(amd64)|(AMD64)")
    check_cxx_compiler_flag("-mavx2" COMPILER_SUPPORTS_AVX2)

    if(COMPILER_SUPPORTS_AVX2)
      target_compile_options(${target} PRIVATE -mavx2)
      message(STATUS "AVX2 SIMD enabled for ${target}")
    else()
      message(WARNING "AVX2 not supported by compiler")
    endif()

  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64|ARM|ARM64")
    # NEON is baseline for AArch64, explicit flag for ARMv7
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm64")
      message(STATUS "NEON SIMD enabled for ${target} (AArch64 baseline)")
    else()
      target_compile_options(${target} PRIVATE -mfpu=neon)
      message(STATUS "NEON SIMD enabled for ${target} (ARMv7)")
    endif()
  endif()
endfunction()

# Add the SIMD kernels to a library target. On x86_64 the AVX2 kernels go into
# their own source file, the only one compiled with -mavx2, and the target
# defines NVECD_HAVE_AVX2_KERNELS so the dispatcher knows they were built.
function(nvecd_add_simd_kernels target)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86)|(X86)|(amd64)|(AMD64)")
    check_cxx_compiler_flag("-mavx2" COMPILER_SUPPORTS_AVX2)

    if(COMPILER_SUPPORTS_AVX2)
      set(kernel_source ${CMAKE_SOURCE_DIR}/src/vectors/distance_avx2_kernels.cpp)
      target_sources(${target} PRIVATE ${kernel_source})
      set_source_files_properties(${kernel_source} PROPERTIES COMPILE_OPTIONS -mavx2)
      target_compile_definitions(${target} PRIVATE NVECD_HAVE_AVX2_KERNELS)
      message(STATUS "AVX2 kernels enabled for ${target}")
    else()
      message(WARNING "AVX2 not supported by compiler")
    endif()
  else()
    nvecd_target_simd_flags(${target})
  endif()
endfunction()
