cmake_minimum_required(VERSION 3.24)
project(NVMatrixEngine LANGUAGES C CXX)
if(NOT WIN32)
  message(FATAL_ERROR "Use the Windows x64 MSVC toolchain through build.ps1.")
endif()
set(CMAKE_CXX_STANDARD 20)
option(NVMATRIXENGINE_SPACETIME_EXPERIMENT "Build the isolated, unfinished ST-FLIP hardware fixture" OFF)
option(NVMATRIXENGINE_CUDA_FLUID "Build opt-in CUDA fluid infrastructure and hardware tests" OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(DEPS "${CMAKE_CURRENT_SOURCE_DIR}/../shared/.deps")
set(RMLUI "${DEPS}/RmlUi-6.3")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
foreach(dep ZLIB BZIP2 PNG HARFBUZZ BROTLI)
  set(FT_DISABLE_${dep} ON CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${DEPS}/freetype-VER-2-14-1" freetype EXCLUDE_FROM_ALL)
add_library(Freetype::Freetype ALIAS freetype)
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory("${RMLUI}" rmlui EXCLUDE_FROM_ALL)
file(GLOB_RECURSE BULLET_SOURCE CONFIGURE_DEPENDS "${DEPS}/bullet/src/LinearMath/*.cpp" "${DEPS}/bullet/src/BulletCollision/*.cpp" "${DEPS}/bullet/src/BulletDynamics/*.cpp")
add_library(lab_bullet STATIC ${BULLET_SOURCE})
target_include_directories(lab_bullet PUBLIC "${DEPS}/bullet/src")
target_compile_definitions(lab_bullet PRIVATE _CRT_SECURE_NO_WARNINGS)
add_executable(NVMatrixFluidLab WIN32 src/main.cpp src/renderer.cpp src/scene.cpp src/gameplay.cpp src/hud.cpp src/streamline.cpp src/fluid/fluid_system.cpp src/fluid/fluid_surface.cpp src/fluid/fluid_surface_lod_validation.cpp src/fluid/fluid_complexity.cpp src/fluid/fluid_pressure.cpp src/fluid/fluid_bulk.cpp src/fluid/fluid_resampling.cpp src/fluid/fluid_interior.cpp src/fluid/whitewater.cpp
  src/fluid/fluid_mac.cpp src/fluid/fluid_mac_validation.cpp
  src/fluid/fluid_mac_pressure.cpp src/fluid/fluid_mac_pressure_validation.cpp
  src/fluid/fluid_work.cpp src/fluid/fluid_work_validation.cpp
  src/fluid/fluid_cut_cells.cpp src/fluid/fluid_cut_cells_validation.cpp src/fluid/fluid_volume_allocator.cpp
  src/fluid/fluid_flux_limiter.cpp
  src/fluid/fluid_bulk_pressure.cpp
  src/fluid/fluid_implicit_transport.cpp
  src/fluid/fluid_carrier_projection.cpp
  src/fluid/fluid_particle_grid_exchange.cpp
  src/fluid/fluid_flowing_band.cpp
  src/optical_importance.cpp
  src/fluid/fluid_buoyancy.cpp
  ../shared/src/game.cpp ../shared/src/ui_renderer.cpp ../shared/src/audio.cpp
  "${RMLUI}/Backends/RmlUi_Platform_Win32.cpp")
set_source_files_properties(../shared/src/audio.cpp PROPERTIES COMPILE_OPTIONS /W0)
target_include_directories(NVMatrixFluidLab BEFORE PRIVATE "${DEPS}/agility-1.619.5/build/native/include")
target_include_directories(NVMatrixFluidLab PRIVATE "${DEPS}/include" "${DEPS}/nvapi" ../shared/src "${RMLUI}/Backends" "${DEPS}/miniaudio-0.11.25")
target_compile_definitions(NVMatrixFluidLab PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(NVMatrixFluidLab SYSTEM PRIVATE "${DEPS}/pix-1.0.240308001/Include/WinPixEventRuntime")
target_compile_definitions(NVMatrixFluidLab PRIVATE USE_PIX)
target_link_libraries(NVMatrixFluidLab PRIVATE "${DEPS}/pix-1.0.240308001/bin/x64/WinPixEventRuntime.lib")
add_custom_command(TARGET NVMatrixFluidLab POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEPS}/pix-1.0.240308001/bin/x64/WinPixEventRuntime.dll" "$<TARGET_FILE_DIR:NVMatrixFluidLab>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEPS}/pix-1.0.240308001/license.txt" "$<TARGET_FILE_DIR:NVMatrixFluidLab>/PIX-LICENSE.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEPS}/pix-1.0.240308001/ThirdPartyNotices.txt" "$<TARGET_FILE_DIR:NVMatrixFluidLab>/PIX-ThirdPartyNotices.txt")
target_compile_options(NVMatrixFluidLab PRIVATE /W4 /EHsc /utf-8)
target_link_libraries(NVMatrixFluidLab PRIVATE lab_bullet RmlUi::Core d3d12 dxgi dxguid d3dcompiler wintrust crypt32 shell32 imm32 ole32 windowscodecs "${DEPS}/nvapi/amd64/nvapi64.lib")
set_target_properties(NVMatrixFluidLab PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(dll sl.interposer sl.common sl.dlss_d sl.dlss nvngx_dlss nvngx_dlssd sl.dlss_g nvngx_dlssg sl.reflex sl.pcl)
  add_custom_command(TARGET NVMatrixFluidLab POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEPS}/bin/x64/${dll}.dll" "$<TARGET_FILE_DIR:NVMatrixFluidLab>")
endforeach()
add_custom_command(TARGET NVMatrixFluidLab POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/ui" "$<TARGET_FILE_DIR:NVMatrixFluidLab>/ui"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RMLUI}/Samples/basic/harfbuzz/data/Poppins-Regular.ttf" "$<TARGET_FILE_DIR:NVMatrixFluidLab>/ui/Poppins-Regular.ttf"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/../shared/assets/audio" "$<TARGET_FILE_DIR:NVMatrixFluidLab>/assets/audio"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:NVMatrixFluidLab>/shaders"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../shared/shaders/ui.hlsl" "$<TARGET_FILE_DIR:NVMatrixFluidLab>/shaders/ui.hlsl"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:NVMatrixFluidLab>/D3D12"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:NVMatrixFluidLab>/assets"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEPS}/agility-1.619.5/build/native/bin/x64/D3D12Core.dll" "$<TARGET_FILE_DIR:NVMatrixFluidLab>/D3D12"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEPS}/agility-1.619.5/build/native/bin/x64/d3d12SDKLayers.dll" "$<TARGET_FILE_DIR:NVMatrixFluidLab>/D3D12"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEPS}/CIE_xyz_1931_2deg.csv" "$<TARGET_FILE_DIR:NVMatrixFluidLab>/assets"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEPS}/license.txt" "$<TARGET_FILE_DIR:NVMatrixFluidLab>/Streamline-LICENSE.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEPS}/nvapi/License.txt" "$<TARGET_FILE_DIR:NVMatrixFluidLab>/NVAPI-LICENSE.txt")
add_custom_command(TARGET NVMatrixFluidLab POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEPS}/rtxdi-f12037fa/LICENSE.txt" "$<TARGET_FILE_DIR:NVMatrixFluidLab>/RTXDI-LICENSE.txt")
foreach(entry "bullet/LICENSE.txt|Bullet" "RmlUi-6.3/LICENSE.txt|RmlUi" "freetype-VER-2-14-1/docs/FTL.TXT|FreeType" "miniaudio-0.11.25/LICENSE|miniaudio" "RmlUi-6.3/Samples/basic/harfbuzz/data/LICENSE.txt|Font")
  string(REPLACE "|" ";" parts "${entry}")
  list(GET parts 0 source)
  list(GET parts 1 name)
  add_custom_command(TARGET NVMatrixFluidLab POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEPS}/${source}" "$<TARGET_FILE_DIR:NVMatrixFluidLab>/${name}-LICENSE.txt")
endforeach()
enable_testing()
add_test(NAME lab_gameplay COMMAND NVMatrixFluidLab --self-test)
add_executable(NVMatrixEngineOrbitTest tests/orbit.cpp)
add_test(NAME lab_orbit_input COMMAND NVMatrixEngineOrbitTest)
add_executable(NVMatrixEngineRollingTest tests/rolling.cpp src/gameplay.cpp src/scene.cpp ../shared/src/game.cpp)
target_include_directories(NVMatrixEngineRollingTest PRIVATE ../shared/src)
target_compile_options(NVMatrixEngineRollingTest PRIVATE /EHsc /utf-8)
target_link_libraries(NVMatrixEngineRollingTest PRIVATE lab_bullet)
add_test(NAME lab_rolling_camera COMMAND NVMatrixEngineRollingTest)
add_executable(NVMatrixEngineExperienceTest tests/experience.cpp src/gameplay.cpp src/scene.cpp ../shared/src/game.cpp)
target_include_directories(NVMatrixEngineExperienceTest PRIVATE ../shared/src)
target_compile_options(NVMatrixEngineExperienceTest PRIVATE /EHsc /utf-8)
target_link_libraries(NVMatrixEngineExperienceTest PRIVATE lab_bullet)
add_test(NAME lab_camera_and_watercraft COMMAND NVMatrixEngineExperienceTest)
add_executable(NVMatrixEngineWaterTest tests/water.cpp src/scene.cpp)
target_compile_options(NVMatrixEngineWaterTest PRIVATE /EHsc /utf-8)
add_test(NAME lab_water_optics COMMAND NVMatrixEngineWaterTest)
add_executable(NVMatrixEngineFluidMeshTest tests/fluid_mesh.cpp src/scene.cpp)
target_compile_definitions(NVMatrixEngineFluidMeshTest PRIVATE NOMINMAX)
target_compile_options(NVMatrixEngineFluidMeshTest PRIVATE /EHsc /utf-8)
add_test(NAME lab_fluid_mesh_sdf COMMAND NVMatrixEngineFluidMeshTest)
add_executable(NVMatrixEngineFluidComplexityTest tests/fluid_complexity.cpp)
add_test(NAME lab_fluid_complexity_policy COMMAND NVMatrixEngineFluidComplexityTest)
add_executable(NVMatrixEngineFluidColliderTest tests/fluid_colliders.cpp)
target_compile_definitions(NVMatrixEngineFluidColliderTest PRIVATE NOMINMAX)
target_compile_options(NVMatrixEngineFluidColliderTest PRIVATE /W4 /EHsc /utf-8)
add_test(NAME lab_fluid_collider_timeline COMMAND NVMatrixEngineFluidColliderTest)
add_executable(NVMatrixEngineSubmissionTest tests/submission_profile.cpp)
target_compile_options(NVMatrixEngineSubmissionTest PRIVATE /W4 /EHsc /utf-8)
add_test(NAME lab_cpu_submission_timeline COMMAND NVMatrixEngineSubmissionTest)

add_executable(NVMatrixEngineUiUploadTest tests/ui_upload_gpu.cpp ../shared/src/ui_renderer.cpp)
target_include_directories(NVMatrixEngineUiUploadTest BEFORE PRIVATE "${DEPS}/agility-1.619.5/build/native/include")
target_include_directories(NVMatrixEngineUiUploadTest PRIVATE ../shared/src "${DEPS}/include" "${DEPS}/bullet/src")
target_include_directories(NVMatrixEngineUiUploadTest SYSTEM PRIVATE "${DEPS}/pix-1.0.240308001/Include/WinPixEventRuntime")
target_compile_definitions(NVMatrixEngineUiUploadTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(NVMatrixEngineUiUploadTest PRIVATE /W4 /EHsc /utf-8)
target_link_libraries(NVMatrixEngineUiUploadTest PRIVATE RmlUi::Core d3d12 dxgi dxguid d3dcompiler ole32 windowscodecs)
set_target_properties(NVMatrixEngineUiUploadTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# Explicit GPU fixture: run from the lab runtime, with its folder as argv[1].

# Explicit hardware test, not part of the hardware-independent CTest suite.
# Run serially with a built lab runtime folder as the single argument.
add_executable(NVMatrixEngineCarrierGpuTest tests/fluid_carrier_gpu.cpp src/fluid/fluid_carrier_projection.cpp
  src/fluid/fluid_mac_pressure.cpp src/fluid/fluid_mac_pressure_validation.cpp)
target_include_directories(NVMatrixEngineCarrierGpuTest BEFORE PRIVATE "${DEPS}/agility-1.619.5/build/native/include")
target_include_directories(NVMatrixEngineCarrierGpuTest PRIVATE "${DEPS}/pix-1.0.240308001/Include/WinPixEventRuntime")
target_compile_definitions(NVMatrixEngineCarrierGpuTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(NVMatrixEngineCarrierGpuTest PRIVATE /W4 /EHsc /utf-8)
target_link_libraries(NVMatrixEngineCarrierGpuTest PRIVATE d3d12 dxgi dxguid d3dcompiler)
set_target_properties(NVMatrixEngineCarrierGpuTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_executable(NVMatrixEngineExchangeGpuTest tests/fluid_exchange_gpu.cpp
  tests/fluid_flowing_band_gpu.cpp src/fluid/fluid_flowing_band.cpp
  src/fluid/fluid_particle_grid_exchange.cpp src/fluid/fluid_implicit_transport.cpp)
target_include_directories(NVMatrixEngineExchangeGpuTest BEFORE PRIVATE "${DEPS}/agility-1.619.5/build/native/include")
target_include_directories(NVMatrixEngineExchangeGpuTest PRIVATE "${DEPS}/pix-1.0.240308001/Include/WinPixEventRuntime")
target_compile_definitions(NVMatrixEngineExchangeGpuTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(NVMatrixEngineExchangeGpuTest PRIVATE /W4 /EHsc /utf-8)
target_link_libraries(NVMatrixEngineExchangeGpuTest PRIVATE d3d12 dxgi dxguid d3dcompiler)
set_target_properties(NVMatrixEngineExchangeGpuTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# This fixture is not a playable ST-FLIP solver. Keep its unvalidated temporal
# transfer/advection work separate from the single-phase CUDA baseline.
if(NVMATRIXENGINE_SPACETIME_EXPERIMENT)
  add_executable(NVMatrixEngineSpacetimeGpuTest tests/fluid_spacetime_gpu.cpp src/fluid/fluid_spacetime.cpp)
  target_include_directories(NVMatrixEngineSpacetimeGpuTest BEFORE PRIVATE "${DEPS}/agility-1.619.5/build/native/include")
  target_include_directories(NVMatrixEngineSpacetimeGpuTest PRIVATE "${DEPS}/pix-1.0.240308001/Include/WinPixEventRuntime")
  target_compile_definitions(NVMatrixEngineSpacetimeGpuTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(NVMatrixEngineSpacetimeGpuTest PRIVATE /W4 /EHsc /utf-8)
  target_link_libraries(NVMatrixEngineSpacetimeGpuTest PRIVATE d3d12 dxgi dxguid d3dcompiler)
  set_target_properties(NVMatrixEngineSpacetimeGpuTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# CUDA is not discovered or required by ordinary DX12-only configurations.
if(NVMATRIXENGINE_CUDA_FLUID)
  # DXC's default include handler needs a native UNC prefix when the repository
  # is in WSL. Forward-slash //wsl inputs compile only shaders without includes.
  file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fluid" CUDA_SHADER_DIR)
  if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES 86 89 120)
  endif()
  enable_language(CUDA)
  find_package(CUDAToolkit 13.0 REQUIRED)
  add_library(lab_cuda_interop STATIC src/gpu_cuda_interop.cpp)
  target_include_directories(lab_cuda_interop BEFORE PRIVATE "${DEPS}/agility-1.619.5/build/native/include")
  target_include_directories(lab_cuda_interop PUBLIC "${DEPS}/pix-1.0.240308001/Include/WinPixEventRuntime")
  target_compile_definitions(lab_cuda_interop PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(lab_cuda_interop PRIVATE /W4 /EHsc /utf-8)
  target_link_libraries(lab_cuda_interop PUBLIC CUDA::cudart_static CUDA::cuda_driver d3d12 dxgi dxguid)

  add_library(lab_cuda_transfer STATIC src/fluid/cuda/fluid_cuda_transfer.cu
    src/fluid/cuda/fluid_cuda_grid.cu src/fluid/cuda/fluid_cuda_collision.cu
    src/fluid/cuda/fluid_cuda_kernels.cu src/fluid/cuda/fluid_cuda_bricks.cu
    src/fluid/cuda/fluid_cuda_mac.cu src/fluid/cuda/fluid_cuda_pressure.cu src/fluid/cuda/fluid_cuda_pressure_loop.cu
    src/fluid/cuda/fluid_cuda_transaction.cu src/fluid/cuda/fluid_cuda_ownership.cu
    src/fluid/cuda/fluid_cuda_owned_transport.cu src/fluid/cuda/fluid_cuda_joint.cu
    src/fluid/cuda/fluid_cuda_geometric_transport.cu src/fluid/cuda/fluid_cuda_narrow_band.cu)
  set_target_properties(lab_cuda_transfer PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON CUDA_RUNTIME_LIBRARY Static)
  target_sources(NVMatrixFluidLab PRIVATE src/fluid/fluid_cuda.cpp)
  target_compile_definitions(NVMatrixFluidLab PRIVATE PT_FLUID_CUDA=1)
  target_link_libraries(NVMatrixFluidLab PRIVATE lab_cuda_interop lab_cuda_transfer)
  add_executable(NVMatrixEngineCudaBrickTest tests/cuda_bricks_gpu.cu)
  set_target_properties(NVMatrixEngineCudaBrickTest PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON CUDA_RUNTIME_LIBRARY Static RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  target_link_libraries(NVMatrixEngineCudaBrickTest PRIVATE lab_cuda_transfer CUDA::cudart_static)
  add_executable(NVMatrixEngineCudaMacTest tests/cuda_mac_gpu.cu)
  set_target_properties(NVMatrixEngineCudaMacTest PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON CUDA_RUNTIME_LIBRARY Static RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  target_link_libraries(NVMatrixEngineCudaMacTest PRIVATE lab_cuda_transfer CUDA::cudart_static)
  add_executable(NVMatrixEngineCudaSolverTest tests/cuda_solver_gpu.cu)
  set_target_properties(NVMatrixEngineCudaSolverTest PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON CUDA_RUNTIME_LIBRARY Static RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  target_link_libraries(NVMatrixEngineCudaSolverTest PRIVATE lab_cuda_transfer CUDA::cudart_static)
  add_executable(NVMatrixEngineCudaNarrowBandTest tests/cuda_narrow_band_gpu.cu)
  set_target_properties(NVMatrixEngineCudaNarrowBandTest PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON CUDA_RUNTIME_LIBRARY Static RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  target_link_libraries(NVMatrixEngineCudaNarrowBandTest PRIVATE lab_cuda_transfer CUDA::cudart_static)
  add_executable(NVMatrixEngineCudaOwnedTransportTest tests/cuda_owned_transport_gpu.cu)
  add_executable(NVMatrixEngineCudaJointTransferTest tests/cuda_joint_transfer_gpu.cu)
  set_target_properties(NVMatrixEngineCudaJointTransferTest PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON CUDA_RUNTIME_LIBRARY Static RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  target_link_libraries(NVMatrixEngineCudaJointTransferTest PRIVATE lab_cuda_transfer CUDA::cudart_static)
  add_executable(NVMatrixEngineCudaGeometricTransportTest tests/cuda_geometric_transport_gpu.cu)
  set_target_properties(NVMatrixEngineCudaGeometricTransportTest PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON CUDA_RUNTIME_LIBRARY Static RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  target_link_libraries(NVMatrixEngineCudaGeometricTransportTest PRIVATE lab_cuda_transfer CUDA::cudart_static)
  set_target_properties(NVMatrixEngineCudaOwnedTransportTest PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON CUDA_RUNTIME_LIBRARY Static RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  target_link_libraries(NVMatrixEngineCudaOwnedTransportTest PRIVATE lab_cuda_transfer CUDA::cudart_static)
  add_executable(NVMatrixEngineCudaExchangeTest tests/fluid_exchange_gpu.cpp src/fluid/fluid_particle_grid_exchange.cpp)
  target_include_directories(NVMatrixEngineCudaExchangeTest BEFORE PRIVATE "${DEPS}/agility-1.619.5/build/native/include")
  target_compile_definitions(NVMatrixEngineCudaExchangeTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN PT_EXCHANGE_CUDA=1)
  target_compile_options(NVMatrixEngineCudaExchangeTest PRIVATE /W4 /EHsc /utf-8)
  target_link_libraries(NVMatrixEngineCudaExchangeTest PRIVATE lab_cuda_interop lab_cuda_transfer CUDA::cudart_static d3dcompiler)
  set_target_properties(NVMatrixEngineCudaExchangeTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  add_custom_command(TARGET NVMatrixEngineCudaExchangeTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:NVMatrixEngineCudaExchangeTest>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEPS}/agility-1.619.5/build/native/bin/x64/D3D12Core.dll" "$<TARGET_FILE_DIR:NVMatrixEngineCudaExchangeTest>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEPS}/agility-1.619.5/build/native/bin/x64/d3d12SDKLayers.dll" "$<TARGET_FILE_DIR:NVMatrixEngineCudaExchangeTest>/D3D12")
  # Standalone scheduling regression also isolates conditional-graph tooling
  # failures from the pressure operator and DX12 interoperability.
  add_executable(NVMatrixEngineCudaConditionalTest tests/cuda_conditional_gpu.cu)
  set_target_properties(NVMatrixEngineCudaConditionalTest PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON CUDA_RUNTIME_LIBRARY Static RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  target_link_libraries(NVMatrixEngineCudaConditionalTest PRIVATE CUDA::cudart_static)
  # Run-time binary instrumentation of cudaGraphSetConditional fails on the
  # tested Windows/Blackwell driver. Compile-time memcheck is an independent,
  # hybrid instrumented route, not a suppression. These explicit test-only
  # targets cannot contaminate the game's library or performance executable.
  if(CUDAToolkit_VERSION VERSION_GREATER_EQUAL "13.1")
    get_target_property(CUDA_TRANSFER_SOURCES lab_cuda_transfer SOURCES)
    list(REMOVE_ITEM CUDA_TRANSFER_SOURCES src/fluid/cuda/fluid_cuda_pressure_loop.cu)
    add_library(lab_cuda_loop_memcheck OBJECT EXCLUDE_FROM_ALL src/fluid/cuda/fluid_cuda_pressure_loop.cu)
    set_target_properties(lab_cuda_loop_memcheck PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON CUDA_RUNTIME_LIBRARY Static)
    target_compile_options(lab_cuda_loop_memcheck PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-fdevice-sanitize=memcheck;-lineinfo>)
    add_library(lab_cuda_transfer_memcheck STATIC EXCLUDE_FROM_ALL ${CUDA_TRANSFER_SOURCES})
    set_target_properties(lab_cuda_transfer_memcheck PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON CUDA_RUNTIME_LIBRARY Static)
    target_link_libraries(lab_cuda_transfer_memcheck PRIVATE lab_cuda_loop_memcheck)
    foreach(fixture Conditional Mac Solver GeometricTransport)
      string(TOLOWER "${fixture}" fixture_lower)
      if(fixture STREQUAL "GeometricTransport")
        set(fixture_lower geometric_transport)
      endif()
      add_executable(NVMatrixEngineCuda${fixture}MemcheckTest EXCLUDE_FROM_ALL tests/cuda_${fixture_lower}_gpu.cu)
      set_target_properties(NVMatrixEngineCuda${fixture}MemcheckTest PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON CUDA_RUNTIME_LIBRARY Static RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
      target_link_libraries(NVMatrixEngineCuda${fixture}MemcheckTest PRIVATE CUDA::cudart_static)
      if(fixture STREQUAL "Conditional")
        target_compile_options(NVMatrixEngineCuda${fixture}MemcheckTest PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-fdevice-sanitize=memcheck;-lineinfo>)
      else()
        target_link_libraries(NVMatrixEngineCuda${fixture}MemcheckTest PRIVATE lab_cuda_transfer_memcheck)
      endif()
    endforeach()
  endif()

  add_library(lab_cuda_interop_test_kernel STATIC tests/cuda_interop_kernel.cu)
  set_target_properties(lab_cuda_interop_test_kernel PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON CUDA_RUNTIME_LIBRARY Static)
  add_executable(NVMatrixEngineCudaInteropTest tests/cuda_interop_gpu.cpp)
  target_include_directories(NVMatrixEngineCudaInteropTest BEFORE PRIVATE "${DEPS}/agility-1.619.5/build/native/include")
  target_compile_definitions(NVMatrixEngineCudaInteropTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(NVMatrixEngineCudaInteropTest PRIVATE /W4 /EHsc /utf-8)
  target_link_libraries(NVMatrixEngineCudaInteropTest PRIVATE lab_cuda_interop lab_cuda_interop_test_kernel d3dcompiler)
  set_target_properties(NVMatrixEngineCudaInteropTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  add_custom_command(TARGET NVMatrixEngineCudaInteropTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:NVMatrixEngineCudaInteropTest>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEPS}/agility-1.619.5/build/native/bin/x64/D3D12Core.dll" "$<TARGET_FILE_DIR:NVMatrixEngineCudaInteropTest>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEPS}/agility-1.619.5/build/native/bin/x64/d3d12SDKLayers.dll" "$<TARGET_FILE_DIR:NVMatrixEngineCudaInteropTest>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:NVMatrixEngineCudaInteropTest>/shaders"
    COMMAND "${DEPS}/dxc-1.9.2607/bin/x64/dxc.exe" -T cs_6_6 -E InteropTest -HV 2021 -O3 "${CUDA_SHADER_DIR}/cuda-interop-test.hlsl" -Fo "$<TARGET_FILE_DIR:NVMatrixEngineCudaInteropTest>/shaders/CudaInteropTest.dxil")

  add_executable(NVMatrixEngineCudaTransferTest tests/cuda_transfer_gpu.cpp)
  target_include_directories(NVMatrixEngineCudaTransferTest BEFORE PRIVATE "${DEPS}/agility-1.619.5/build/native/include")
  target_compile_definitions(NVMatrixEngineCudaTransferTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(NVMatrixEngineCudaTransferTest PRIVATE /W4 /EHsc /utf-8)
  target_link_libraries(NVMatrixEngineCudaTransferTest PRIVATE lab_cuda_interop lab_cuda_transfer d3dcompiler)
  set_target_properties(NVMatrixEngineCudaTransferTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  add_custom_command(TARGET NVMatrixEngineCudaTransferTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:NVMatrixEngineCudaTransferTest>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEPS}/agility-1.619.5/build/native/bin/x64/D3D12Core.dll" "$<TARGET_FILE_DIR:NVMatrixEngineCudaTransferTest>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEPS}/agility-1.619.5/build/native/bin/x64/d3d12SDKLayers.dll" "$<TARGET_FILE_DIR:NVMatrixEngineCudaTransferTest>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:NVMatrixEngineCudaTransferTest>/shaders"
    COMMAND "${DEPS}/dxc-1.9.2607/bin/x64/dxc.exe" -T cs_6_6 -E PerturbFaces -HV 2021 -O3 "${CUDA_SHADER_DIR}/cuda-transfer-test.hlsl" -Fo "$<TARGET_FILE_DIR:NVMatrixEngineCudaTransferTest>/shaders/CudaPerturbFaces.dxil")
  foreach(entry ClearBins CountBins ScanCells ScanSums FinishScan Scatter SortBins P2G G2P)
    if(entry STREQUAL "P2G" OR entry STREQUAL "G2P")
      set(source transfer)
    else()
      set(source binning)
    endif()
    add_custom_command(TARGET NVMatrixEngineCudaTransferTest POST_BUILD
      COMMAND "${DEPS}/dxc-1.9.2607/bin/x64/dxc.exe" -T cs_6_6 -E ${entry} -HV 2021 -O3 "${CUDA_SHADER_DIR}/${source}.hlsl" -Fo "$<TARGET_FILE_DIR:NVMatrixEngineCudaTransferTest>/shaders/Fluid${entry}.dxil")
  endforeach()
  add_executable(NVMatrixEngineCudaCoreTest tests/cuda_core_gpu.cpp)
  target_include_directories(NVMatrixEngineCudaCoreTest BEFORE PRIVATE "${DEPS}/agility-1.619.5/build/native/include")
  target_compile_definitions(NVMatrixEngineCudaCoreTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(NVMatrixEngineCudaCoreTest PRIVATE /W4 /EHsc /utf-8)
  target_link_libraries(NVMatrixEngineCudaCoreTest PRIVATE lab_cuda_interop lab_cuda_transfer d3dcompiler)
  set_target_properties(NVMatrixEngineCudaCoreTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  add_executable(NVMatrixEngineCudaSurfaceTest tests/cuda_surface_gpu.cpp src/fluid/fluid_cuda.cpp
    src/fluid/fluid_surface.cpp src/fluid/fluid_surface_lod_validation.cpp)
  target_include_directories(NVMatrixEngineCudaSurfaceTest BEFORE PRIVATE "${DEPS}/agility-1.619.5/build/native/include")
  target_include_directories(NVMatrixEngineCudaSurfaceTest PRIVATE ../shared/src "${DEPS}/pix-1.0.240308001/Include/WinPixEventRuntime")
  target_compile_definitions(NVMatrixEngineCudaSurfaceTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN PT_FLUID_CUDA=1)
  target_compile_options(NVMatrixEngineCudaSurfaceTest PRIVATE /W4 /EHsc /utf-8)
  target_link_libraries(NVMatrixEngineCudaSurfaceTest PRIVATE lab_cuda_interop lab_cuda_transfer d3dcompiler)
  set_target_properties(NVMatrixEngineCudaSurfaceTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  add_custom_command(TARGET NVMatrixEngineCudaSurfaceTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:NVMatrixEngineCudaSurfaceTest>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEPS}/agility-1.619.5/build/native/bin/x64/D3D12Core.dll" "$<TARGET_FILE_DIR:NVMatrixEngineCudaSurfaceTest>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEPS}/agility-1.619.5/build/native/bin/x64/d3d12SDKLayers.dll" "$<TARGET_FILE_DIR:NVMatrixEngineCudaSurfaceTest>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:NVMatrixEngineCudaSurfaceTest>/shaders")
  foreach(entry SurfaceSeed SurfaceProbe SurfaceBoxProbe SurfaceGeometrySeed SurfaceCurveSeed)
    add_custom_command(TARGET NVMatrixEngineCudaSurfaceTest POST_BUILD
      COMMAND "${DEPS}/dxc-1.9.2607/bin/x64/dxc.exe" -T cs_6_6 -E ${entry} -HV 2021 -O3 "${CUDA_SHADER_DIR}/cuda-surface-test.hlsl" -Fo "$<TARGET_FILE_DIR:NVMatrixEngineCudaSurfaceTest>/shaders/Cuda${entry}.dxil")
  endforeach()
  add_custom_command(TARGET NVMatrixEngineCudaSurfaceTest POST_BUILD
    COMMAND "${DEPS}/dxc-1.9.2607/bin/x64/dxc.exe" -T lib_6_6 -HV 2021 -O3 "${CUDA_SHADER_DIR}/cuda-surface-rays.hlsl" -Fo "$<TARGET_FILE_DIR:NVMatrixEngineCudaSurfaceTest>/shaders/CudaSurfaceRays.dxil")
  add_custom_command(TARGET NVMatrixEngineCudaCoreTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:NVMatrixEngineCudaCoreTest>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEPS}/agility-1.619.5/build/native/bin/x64/D3D12Core.dll" "$<TARGET_FILE_DIR:NVMatrixEngineCudaCoreTest>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEPS}/agility-1.619.5/build/native/bin/x64/d3d12SDKLayers.dll" "$<TARGET_FILE_DIR:NVMatrixEngineCudaCoreTest>/D3D12"
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/compile-cuda-tests.ps1" -OutputDir "$<TARGET_FILE_DIR:NVMatrixEngineCudaCoreTest>/shaders")
  # CUDA's VS dependency tracker can miss case-sensitive WSL include paths.
  # Both supported CUDA build scripts configure explicitly. Bake the complete
  # interface fingerprint into compiler commands so changing a header cannot
  # link a freshly built library against stale CUDA/C++ ABI consumers.
  file(GLOB CUDA_FLUID_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/src/fluid/cuda/*.h"
                               "${CMAKE_CURRENT_SOURCE_DIR}/src/fluid/cuda/*.cuh")
  list(APPEND CUDA_FLUID_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/src/fluid/fluid_uniforms.h"
                                "${CMAKE_CURRENT_SOURCE_DIR}/src/fluid/fluid_colliders.h")
  set(CUDA_FLUID_HEADER_HASHES "")
  foreach(header IN LISTS CUDA_FLUID_HEADERS)
    file(SHA256 "${header}" header_hash)
    string(APPEND CUDA_FLUID_HEADER_HASHES "${header_hash};")
  endforeach()
  string(SHA256 CUDA_FLUID_INTERFACE_HASH "${CUDA_FLUID_HEADER_HASHES}")
  foreach(target lab_cuda_transfer NVMatrixFluidLab NVMatrixEngineCudaTransferTest NVMatrixEngineCudaCoreTest
                 NVMatrixEngineCudaBrickTest NVMatrixEngineCudaMacTest NVMatrixEngineCudaSolverTest NVMatrixEngineCudaNarrowBandTest NVMatrixEngineCudaSurfaceTest NVMatrixEngineCudaOwnedTransportTest NVMatrixEngineCudaExchangeTest NVMatrixEngineCudaJointTransferTest NVMatrixEngineCudaGeometricTransportTest
                 lab_cuda_transfer_memcheck lab_cuda_loop_memcheck NVMatrixEngineCudaMacMemcheckTest NVMatrixEngineCudaSolverMemcheckTest NVMatrixEngineCudaGeometricTransportMemcheckTest)
    if(TARGET ${target})
      target_compile_definitions(${target} PRIVATE "PT_CUDA_ABI_${CUDA_FLUID_INTERFACE_HASH}=1")
    endif()
  endforeach()
endif()
