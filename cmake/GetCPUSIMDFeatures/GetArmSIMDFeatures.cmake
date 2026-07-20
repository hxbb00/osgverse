#[=======================================================================[.rst:
GetArmSIMDFeatures
------------------

  Get feature list for target micro architecture

.. command:: get_arm_simd_features

   get_arm_simd_features(<output variable> <target architecture>)

#]=======================================================================]

function(GET_ARM_SIMD_FEATURES outvar tarch)
    set(_available_vector_units_list)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        file(READ "/proc/cpuinfo" _cpuinfo)
        string(REGEX REPLACE ".*flags[ \t]*:[ \t]+([^\n]+).*" "\\1" _cpu_flags "${_cpuinfo}")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        exec_program("/usr/sbin/sysctl -n machdep.cpu.features" OUTPUT_VARIABLE _sysctl_output_string)
        string(TOLOWER "${_sysctl_output_string}" _cpu_flags)
        string(REPLACE "." "_" _cpu_flags "${_cpu_flags}")
    else()
        set(_cpu_flags)
    endif()

    if(_cpu_flags MATCHES "asimd")
        list(APPEND _available_vector_units_list "neon")
    endif()
    if(_cpu_flags MATCHES "asimdhp")
        list(APPEND _available_vector_units_list "fp16")
    endif()
    if(_cpu_flags MATCHES "asimddp")
        list(APPEND _available_vector_units_list "int8dot")
    endif()
    set(${outvar} ${_available_vector_units_list} PARENT_SCOPE)
endfunction()
