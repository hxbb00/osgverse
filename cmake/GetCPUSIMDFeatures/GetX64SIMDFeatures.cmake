# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

#[=======================================================================[.rst:
GetX64SIMDFeatures
------------------

  Get feature list for target Intel micro architecture

.. command:: get_x64_simd_features

   get_x64_simd_features(<output variable> <target architecture>)

 supported targets are: "none", "generic", "core", "merom" (65nm Core2),
    "penryn" (45nm Core2), "nehalem", "westmere", "sandy-bridge", "ivy-bridge",
    "haswell", "broadwell", "skylake", "skylake-xeon", "kabylake", "coffelake",
    "cannonlake", "silvermont", "rocketlake", "tigerlake"
    "goldmont", "knl" (Knights Landing), "atom", "k8", "k8-sse3", "barcelona",
    "istanbul", "magny-cours", "bulldozer", "interlagos", "piledriver",
    "AMD-14h", "AMD-16h", "zen".
#]=======================================================================]

function(GET_X64_SIMD_FEATURES outvar tarch)
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

    if(_cpu_flags MATCHES "sse")
        list(APPEND _available_vector_units_list "sse")
    endif()
    if(_cpu_flags MATCHES "sse2")
        list(APPEND _available_vector_units_list "sse2")
    endif()
    if(_cpu_flags MATCHES "sse3")
        list(APPEND _available_vector_units_list "sse3")
    endif()
    if(_cpu_flags MATCHES "ssse3")
        list(APPEND _available_vector_units_list "ssse3")
    endif()
    if(_cpu_flags MATCHES "sse4_1")
        list(APPEND _available_vector_units_list "sse4.1")
    endif()
    if(_cpu_flags MATCHES "sse4_2")
        list(APPEND _available_vector_units_list "sse4.2")
    endif()
    if(_cpu_flags MATCHES "avx")
        list(APPEND _available_vector_units_list "avx")
    endif()
    if(_cpu_flags MATCHES "avx2")
        list(APPEND _available_vector_units_list "avx2")
    endif()
    if(_cpu_flags MATCHES "avx512")
        list(APPEND _available_vector_units_list "avx512")
    endif()
    if(_cpu_flags MATCHES "fma")
        list(APPEND _available_vector_units_list "fma")
    endif()
    set(${outvar} ${_available_vector_units_list} PARENT_SCOPE)

    list(LENGTH _available_vector_units_list vector_length)
    if(vector_length EQUAL 0)
        # https://gcc.gnu.org/onlinedocs/gcc-16.2.0/gcc/x86-Options.html
        set(_isa_sse2    "sse" "sse2")
        set(_isa_sse3    "sse3")                       # Prescott / Yonah
        set(_isa_ssse3   "ssse3")                      # Core 2 (Merom)
        set(_isa_sse4_1  "sse4.1")                     # Penryn
        set(_isa_sse4_2  "sse4.2")                     # Nehalem
        set(_isa_avx     "avx")                        # Sandy Bridge
        set(_isa_f16c    "f16c")                       # Ivy Bridge
        set(_isa_rdrnd   "rdrnd")                      # Ivy Bridge
        set(_isa_avx2    "avx2" "fma" "bmi" "bmi2")    # Haswell
        set(_isa_avx512  "avx512f" "avx512cd" "avx512dq" "avx512bw" "avx512vl")  # Skylake-X
        set(_isa_avx512_vnni "avx512ifma" "avx512vbmi")  # Cannon Lake / Ice Lake

        # Intel Core 2
        set(_arch_core       ${_isa_sse2} ${_isa_sse3})
        set(_arch_merom      ${_arch_core} ${_isa_ssse3})
        set(_arch_penryn     ${_arch_merom} ${_isa_sse4_1})
        # Intel Nehalem
        set(_arch_nehalem    ${_arch_penryn} ${_isa_sse4_2})
        set(_arch_westmere   ${_arch_nehalem})
        # Intel Sandy Bridge
        set(_arch_sandy-bridge ${_arch_nehalem} ${_isa_avx})
        set(_arch_ivy-bridge   ${_arch_sandy-bridge} ${_isa_f16c} ${_isa_rdrnd} "avxi")
        # Intel Haswell
        set(_arch_haswell    ${_arch_ivy-bridge} ${_isa_avx2})
        set(_arch_broadwell  ${_arch_haswell})
        set(_arch_skylake    ${_arch_broadwell})
        # Intel Skylake Server
        set(_arch_skylake-xeon   ${_arch_skylake} ${_isa_avx512})
        set(_arch_skylake-avx512 ${_arch_skylake} ${_isa_avx512})
        # Intel Kaby Lake / Coffee Lake / Comet Lake
        set(_arch_kabylake   ${_arch_skylake})
        set(_arch_coffelake  ${_arch_skylake})
        set(_arch_cometlake  ${_arch_skylake})
        # Intel Ice Lake / Tiger Lake / Rocket Lake
        set(_arch_cannonlake ${_arch_skylake} ${_isa_avx512} ${_isa_avx512_vnni})
        set(_arch_tigerlake  ${_arch_cannonlake})
        set(_arch_rocketlake ${_arch_cannonlake})
        # Intel Alder Lake / Raptor Lake
        set(_arch_alderlake  ${_arch_skylake})
        set(_arch_raptorlake ${_arch_skylake})
        # Intel 2020~
        set(_arch_meteorlake ${_arch_alderlake})
        set(_arch_arrowlake  ${_arch_alderlake})        # Arrow Lake S/H/HX/U
        set(_arch_lunarlake  ${_arch_alderlake})        # Lunar Lake
        set(_arch_pantherlake ${_arch_alderlake})       # Panther Lake
        set(_arch_wildcatlake ${_arch_alderlake})       # Wildcat Lake
        set(_arch_bartlettlake ${_arch_alderlake})      # Bartlett Lake
        set(_arch_sapphire-rapids ${_arch_skylake-xeon})   # Sapphire Rapids
        set(_arch_emerald-rapids  ${_arch_sapphire-rapids}) # Emerald Rapids
        set(_arch_granite-rapids  ${_arch_sapphire-rapids}) # Granite Rapids
        set(_arch_clearwater-forest ${_arch_granite-rapids}) # Clearwater Forest
        # Intel Atom
        set(_arch_atom       ${_isa_sse2} ${_isa_sse3} ${_isa_ssse3})
        set(_arch_silvermont ${_arch_atom} ${_isa_sse4_1} ${_isa_sse4_2} ${_isa_rdrnd})
        set(_arch_goldmont   ${_arch_silvermont})
        set(_arch_elkhartlake ${_arch_goldmont})        # Elkhart Lake (Tremont)
        set(_arch_jasperlake  ${_arch_goldmont})        # Jasper Lake (Tremont)
        set(_arch_grandridge  ${_arch_goldmont})        # Grand Ridge (Crestmont)
        # Intel Xeon Phi
        set(_arch_knl  ${_arch_haswell} ${_isa_avx512} "avx512pf" "avx512er")
        set(_arch_knm  ${_arch_knl} "avx512dq" "avx512bw" "avx512vl" ${_isa_avx512_vnni} "avx512_4fmaps")
        # AMD
        set(_arch_amd-k8         ${_isa_sse2})
        set(_arch_amd-k8-sse3    ${_isa_sse2} ${_isa_sse3})
        set(_arch_barcelona      ${_isa_sse2} ${_isa_sse3} "sse4a")
        # AMD K10
        set(_arch_amd-12h    ${_arch_barcelona})
        set(_arch_amd-10h    ${_arch_barcelona})
        # AMD Bulldozer
        set(_isa_amd_avx     "avx" "xop" "fma4" "sse4a")
        set(_arch_bulldozer  ${_isa_sse2} ${_isa_sse3} ${_isa_ssse3} ${_isa_sse4_1} ${_isa_sse4_2} ${_isa_amd_avx})
        set(_arch_piledriver ${_arch_bulldozer} "fma" ${_isa_f16c})
        set(_arch_interlagos ${_arch_bulldozer})
        # AMD Zen
        set(_arch_amd-zen    ${_arch_haswell} "sse4a")
        # AMD 14h - 1ah
        set(_arch_amd-1ah    ${_isa_sse2} ${_isa_sse3} ${_isa_ssse3} "sse4a" ${_isa_sse4_1} ${_isa_sse4_2} "avx" ${_isa_f16c})
        set(_arch_amd-19h    ${_isa_sse2} ${_isa_sse3} ${_isa_ssse3} "sse4a" ${_isa_sse4_1} ${_isa_sse4_2} "avx" ${_isa_f16c})
        set(_arch_amd-18h    ${_isa_sse2} ${_isa_sse3} ${_isa_ssse3} "sse4a" ${_isa_sse4_1} ${_isa_sse4_2} "avx" ${_isa_f16c})
        set(_arch_amd-17h    ${_isa_sse2} ${_isa_sse3} ${_isa_ssse3} "sse4a" ${_isa_sse4_1} ${_isa_sse4_2} "avx" ${_isa_f16c})
        set(_arch_amd-16h    ${_isa_sse2} ${_isa_sse3} ${_isa_ssse3} "sse4a" ${_isa_sse4_1} ${_isa_sse4_2} "avx" ${_isa_f16c})
        set(_arch_amd-15h    ${_isa_sse2} ${_isa_sse3} ${_isa_ssse3} "sse4a")
        set(_arch_amd-14h    ${_isa_sse2} ${_isa_sse3} ${_isa_ssse3} "sse4a")

        if(DEFINED _arch_${tarch})
            set(${outvar} ${_arch_${tarch}} PARENT_SCOPE)
        elseif(tarch STREQUAL "generic" OR
               tarch STREQUAL "none")
            set(${outvar} "" PARENT_SCOPE)
        else()
            message(WARNING "Unknown target architecture: \"${tarch}\".")
            set(${outvar} "-NOT-FOUND" PARENT_SCOPE)
        endif()
    endif()
endfunction()
