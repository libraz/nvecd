# ISA flags for the targets that compile the SIMD distance kernels.
#
# The kernels are selected with #ifdef __AVX2__ / __ARM_NEON, so a target built
# without the flag does not fail to link -- it silently compiles the branch
# away. That makes this a shared function rather than a block repeated per
# target: the library that defines a kernel and the test that asserts it have
# to agree, and a test built without the flag reports success for a kernel it
# never called. On x86_64 the global architecture flag is a baseline
# (-march=x86-64) in a portable build, so nothing else supplies -mavx2.

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
