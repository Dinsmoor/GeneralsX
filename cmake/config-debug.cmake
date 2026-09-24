set(RTS_DEBUG_LOGGING "DEFAULT" CACHE STRING "Enables debug logging. When DEFAULT, this option is enabled with DEBUG or INTERNAL")
set_property(CACHE RTS_DEBUG_LOGGING PROPERTY STRINGS DEFAULT ON OFF)

set(RTS_DEBUG_CRASHING "DEFAULT" CACHE STRING "Enables debug assert dialogs. When DEFAULT, this option is enabled with DEBUG or INTERNAL")
set_property(CACHE RTS_DEBUG_CRASHING PROPERTY STRINGS DEFAULT ON OFF)

set(RTS_DEBUG_STACKTRACE "DEFAULT" CACHE STRING "Enables debug stacktracing. This inherintly also enables debug logging. When DEFAULT, this option is enabled with DEBUG or INTERNAL")
set_property(CACHE RTS_DEBUG_STACKTRACE PROPERTY STRINGS DEFAULT ON OFF)

set(RTS_DEBUG_PROFILE "DEFAULT" CACHE STRING "Enables debug profiling. When DEFAULT, this option is enabled with DEBUG or INTERNAL")
set_property(CACHE RTS_DEBUG_PROFILE PROPERTY STRINGS DEFAULT ON OFF)

option(RTS_DEBUG_CHEATS "Enables debug cheats in release builds" OFF)
option(RTS_DEBUG_INCLUDE_DEBUG_LOG_IN_CRC_LOG "Includes normal debug log in crc log" OFF)
option(RTS_DEBUG_MULTI_INSTANCE "Enables multi client instance support" OFF)

# Per-generator random-draw tracing. Each logs every draw with its frame,
# value, range, FILE and LINE, which is how you find a desync caused by one
# peer taking a draw the others did not: build both peers with the same option,
# play the match, then diff the logs -- the first differing line names the call
# site. All three need RTS_DEBUG_LOGGING=ON to produce any output at all.
#
# RANDOM_LOGIC is the one that matters for a lockstep mismatch, since only the
# LOGIC generator feeds the CRC. All are OFF by default and are very chatty:
# combat is thousands of draws per second, so these are for a hunt, not a
# habit.
option(RTS_DEBUG_RANDOM_LOGIC "Logs every GameLogic random draw with file and line" OFF)
option(RTS_DEBUG_RANDOM_CLIENT "Logs every GameClient random draw with file and line" OFF)
option(RTS_DEBUG_RANDOM_AUDIO "Logs every GameAudio random draw with file and line" OFF)
# Seeds all three generators with 0 instead of the wall clock, so successive
# runs replay the same sequence.
option(RTS_DEBUG_DETERMINISTIC_RANDOM "Seeds the random generators deterministically" OFF)


define_tristate_option(RTS_DEBUG_LOGGING    DebugLogging    "Build with Debug Logging"      DEBUG_LOGGING    DISABLE_DEBUG_LOGGING)
define_tristate_option(RTS_DEBUG_CRASHING   DebugCrashing   "Build with Debug Crashing"     DEBUG_CRASHING   DISABLE_DEBUG_CRASHING)
define_tristate_option(RTS_DEBUG_STACKTRACE DebugStacktrace "Build with Debug Stacktracing" DEBUG_STACKTRACE DISABLE_DEBUG_STACKTRACE)
define_tristate_option(RTS_DEBUG_PROFILE    DebugProfile    "Build with Debug Profiling"    DEBUG_PROFILE    DISABLE_DEBUG_PROFILE)

add_feature_info(DebugCheats RTS_DEBUG_CHEATS "Build with Debug Cheats in release builds")
add_feature_info(DebugIncludeDebugLogInCrcLog RTS_DEBUG_INCLUDE_DEBUG_LOG_IN_CRC_LOG "Build with Debug Logging in CRC log")
add_feature_info(DebugMultiInstance RTS_DEBUG_MULTI_INSTANCE "Build with Multi Client Instance support")
add_feature_info(DebugRandomLogic RTS_DEBUG_RANDOM_LOGIC "Build with GameLogic random draw tracing")
add_feature_info(DebugRandomClient RTS_DEBUG_RANDOM_CLIENT "Build with GameClient random draw tracing")
add_feature_info(DebugRandomAudio RTS_DEBUG_RANDOM_AUDIO "Build with GameAudio random draw tracing")
add_feature_info(DebugDeterministicRandom RTS_DEBUG_DETERMINISTIC_RANDOM "Build with deterministically seeded random")


if(RTS_DEBUG_CHEATS)
    target_compile_definitions(core_config INTERFACE _ALLOW_DEBUG_CHEATS_IN_RELEASE)
endif()

if(RTS_DEBUG_INCLUDE_DEBUG_LOG_IN_CRC_LOG)
    target_compile_definitions(core_config INTERFACE INCLUDE_DEBUG_LOG_IN_CRC_LOG)
endif()

if(RTS_DEBUG_MULTI_INSTANCE)
    target_compile_definitions(core_config INTERFACE RTS_MULTI_INSTANCE)
endif()

if(RTS_DEBUG_RANDOM_LOGIC)
    target_compile_definitions(core_config INTERFACE DEBUG_RANDOM_LOGIC)
endif()

if(RTS_DEBUG_RANDOM_CLIENT)
    target_compile_definitions(core_config INTERFACE DEBUG_RANDOM_CLIENT)
endif()

if(RTS_DEBUG_RANDOM_AUDIO)
    target_compile_definitions(core_config INTERFACE DEBUG_RANDOM_AUDIO)
endif()

if(RTS_DEBUG_DETERMINISTIC_RANDOM)
    target_compile_definitions(core_config INTERFACE DETERMINISTIC)
endif()
