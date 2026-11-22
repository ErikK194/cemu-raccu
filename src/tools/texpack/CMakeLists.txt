# Texture packs: standalone tools. None of these targets is part of the emulator binary; they reuse the
# emulator-neutral texture-pack modules (docs/texpack/SPEC.md).

# Validates KTX2 files and packs with the same loader code the emulator uses (check, scan, dump, selftest)
add_executable(texpack_ktx2_tool
	texpack_ktx2_tool.cpp
	../../Cafe/HW/Latte/TexturePack/TexPackKtx2Loader.cpp
	../../Cafe/HW/Latte/TexturePack/TexPackKtx2Pack.cpp
	../../Cafe/HW/Latte/TexturePack/TexPackKey.cpp
)
target_include_directories(texpack_ktx2_tool PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(texpack_ktx2_tool PRIVATE basisu::basisu_encoder zstd::zstd PNG::PNG xxHash::xxhash)

# Checks the hash-span profile (SPEC.md section 2) against Cemu's GX2 address library for all formats and sizes
add_executable(texpack_hashspan_check
	texpack_hashspan_check.cpp
	../../Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.cpp
	../../Cafe/HW/Latte/LatteAddrLib/LatteAddrLib_Coord.cpp
)
target_include_directories(texpack_hashspan_check PRIVATE "${CMAKE_SOURCE_DIR}/src")
cemu_use_precompiled_header(texpack_hashspan_check)
target_link_libraries(texpack_hashspan_check PRIVATE CemuCommon)

foreach(tool texpack_ktx2_tool texpack_hashspan_check)
	set_target_properties(${tool} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/texpack_tools")
	set_property(TARGET ${tool} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endforeach()
