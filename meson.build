# SPDX-License-Identifier: GPL-3.0-or-later

project('waywal', ['c', 'cpp'],
  version: '3.0.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.61.0',
  default_options: [
    'c_std=gnu2x',
    'cpp_std=c++23',
    'optimization=3',
    'b_lto=true',
    'b_pie=true',
    'warning_level=3',
    'werror=false',
  ]
)

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')
python3 = find_program('python3')

# Optimization & compiler hardening arguments
base_c_args = [
  '-fno-plt',
  '-fvisibility=hidden',
  '-fstrict-aliasing',
  '-D_GNU_SOURCE',
  '-DWAYWAL_VERSION="' + meson.project_version() + '"',
  '-DWWAL_DATADIR="' + (get_option('prefix') / get_option('datadir')) + '"',
]

if get_option('buildtype') != 'release'
  base_c_args += ['-DWWAL_SOURCE_ROOT="' + meson.current_source_dir() + '"']
endif

add_project_arguments(base_c_args, language: 'c')

extra_c_args = cc.get_supported_arguments([
  '-Wundef',
  '-Wmissing-prototypes',
  '-Wstrict-prototypes',
  '-Wold-style-definition',
  '-Wpointer-arith',
  '-Winit-self',
  '-Wimplicit-fallthrough',
  '-Wno-unused-parameter',
  '-fstack-protector-strong',
  '-ffunction-sections',
  '-fdata-sections',
])
add_project_arguments(extra_c_args, language: 'c')

extra_link_args = cc.get_supported_link_arguments([
  '-Wl,--as-needed',
  '-Wl,--gc-sections',
])
add_project_link_arguments(extra_link_args, language: 'c')

base_cpp_args = [
  '-fno-plt',
  '-fstrict-aliasing',
  '-D_GNU_SOURCE',
  '-DWAYWAL_VERSION="' + meson.project_version() + '"',
  '-DWWAL_DATADIR="' + (get_option('prefix') / get_option('datadir')) + '"',
]

if get_option('buildtype') != 'release'
  base_cpp_args += ['-DWWAL_SOURCE_ROOT="' + meson.current_source_dir() + '"']
endif

add_project_arguments(base_cpp_args, language: 'cpp')

extra_cpp_args = cpp.get_supported_arguments([
  '-Wundef',
  '-Wpointer-arith',
  '-Winit-self',
  '-Wimplicit-fallthrough',
  '-Wno-unused-parameter',
  '-fstack-protector-strong',
  '-ffunction-sections',
  '-fdata-sections',
])
if host_machine.cpu_family() == 'x86_64'
  extra_cpp_args += cpp.get_supported_arguments(['-mavx2', '-mfma'])
endif
add_project_arguments(extra_cpp_args, language: 'cpp')

extra_cpp_link_args = cpp.get_supported_link_arguments([
  '-Wl,--as-needed',
  '-Wl,--gc-sections',
])
add_project_link_arguments(extra_cpp_link_args, language: 'cpp')

if not get_option('enable_simd')
  add_project_arguments('-DWWAL_DISABLE_SIMD', language: 'c')
endif

# Core dependencies
wayland_client_dep = dependency('wayland-client', version: '>=1.20')
wayland_protos_dep = dependency('wayland-protocols', version: '>=1.34')
wayland_scanner = find_program('wayland-scanner')
libdrm_dep = dependency('libdrm', version: '>=2.4.110')
gbm_dep = dependency('gbm')
egl_dep = dependency('egl')
glesv2_dep = dependency('glesv2')
threads_dep = dependency('threads')
m_dep = cc.find_library('m', required: false)
dl_dep = cc.find_library('dl', required: false)

# Optional accelerated dependencies
vaapi_dep = dependency('libva', required: get_option('enable_vaapi'))
vaapi_drm_dep = dependency('libva-drm', required: get_option('enable_vaapi'))
liburing_dep = dependency('liburing', required: get_option('enable_io_uring'))
seccomp_dep = dependency('libseccomp', required: get_option('enable_seccomp'))
if seccomp_dep.found()
  add_project_arguments('-DHAVE_LIBSECCOMP', language: 'c')
endif

avcodec_dep = dependency('libavcodec', required: get_option('ffmpeg_swdec'))
avutil_dep = dependency('libavutil', required: get_option('ffmpeg_swdec'))
swscale_dep = dependency('libswscale', required: get_option('ffmpeg_swdec'))
have_ffmpeg_swdec = avcodec_dep.found() and avutil_dep.found() and swscale_dep.found()
if have_ffmpeg_swdec
  add_project_arguments('-DHAVE_FFMPEG_SWDEC=1', language: 'c')
endif

# Optional runtime dlopen plugin probing
webp_dep = dependency('libwebp', required: get_option('webp').enabled())
avif_dep = dependency('libavif', required: get_option('avif').enabled())
jxl_dep = dependency('libjxl', required: get_option('jxl').enabled())
svg_dep = dependency('librsvg-2.0', required: get_option('svg').enabled())
pipewire_dep = dependency('libpipewire-0.3', required: get_option('pipewire').enabled())

if get_option('webp').disabled()
  add_project_arguments('-DWWAL_DISABLE_WEBP', language: 'c')
endif
if get_option('avif').disabled()
  add_project_arguments('-DWWAL_DISABLE_AVIF', language: 'c')
endif
if get_option('jxl').disabled()
  add_project_arguments('-DWWAL_DISABLE_JXL', language: 'c')
endif
if get_option('svg').disabled()
  add_project_arguments('-DWWAL_DISABLE_SVG', language: 'c')
endif
if get_option('pipewire').disabled()
  add_project_arguments('-DWWAL_DISABLE_PIPEWIRE', language: 'c')
endif

# OpenMP for multi-threaded CPU transition engine
openmp_dep = dependency('openmp', required: false, language: 'c')
if openmp_dep.found()
  add_project_arguments('-fopenmp', language: 'c')
  add_project_arguments('-DHAVE_OPENMP', language: 'c')
endif

inc_dir = include_directories('include', 'src/client')

# Build-time GLSL ES 3.1 compute shader embedding
shader_comp_files = files(
  'src/daemon/shaders/burn.comp',
  'src/daemon/shaders/crosszoom.comp',
  'src/daemon/shaders/cube.comp',
  'src/daemon/shaders/dissolve_noise.comp',
  'src/daemon/shaders/doom.comp',
  'src/daemon/shaders/fade.comp',
  'src/daemon/shaders/glitch.comp',
  'src/daemon/shaders/grow.comp',
  'src/daemon/shaders/light_leak.comp',
  'src/daemon/shaders/luma.comp',
  'src/daemon/shaders/outer.comp',
  'src/daemon/shaders/page_curl.comp',
  'src/daemon/shaders/pixelate.comp',
  'src/daemon/shaders/ripple.comp',
  'src/daemon/shaders/slide.comp',
  'src/daemon/shaders/swirl.comp',
  'src/daemon/shaders/wave.comp',
  'src/daemon/shaders/wipe.comp',
)

shaders_embedded_h = custom_target('shaders_embedded.h',
  input: shader_comp_files,
  output: 'shaders_embedded.h',
  command: [python3, files('scripts/embed_shaders.py'), '@OUTPUT@', '@INPUT@'],
)

shaders_dep = declare_dependency(sources: [shaders_embedded_h])

# Wayland protocol code generation helper
wayland_scanner_code = generator(
  wayland_scanner,
  output: '@BASENAME@-protocol.c',
  arguments: ['private-code', '@INPUT@', '@OUTPUT@']
)

wayland_scanner_client = generator(
  wayland_scanner,
  output: '@BASENAME@-client-protocol.h',
  arguments: ['client-header', '@INPUT@', '@OUTPUT@']
)

wl_protocol_dir = wayland_protos_dep.get_variable(pkgconfig: 'pkgdatadir')

protocols = [
  wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
  wl_protocol_dir / 'stable/viewporter/viewporter.xml',
  wl_protocol_dir / 'stable/presentation-time/presentation-time.xml',
  wl_protocol_dir / 'stable/linux-dmabuf/linux-dmabuf-v1.xml',
  wl_protocol_dir / 'staging/fractional-scale/fractional-scale-v1.xml',
  wl_protocol_dir / 'staging/single-pixel-buffer/single-pixel-buffer-v1.xml',
  wl_protocol_dir / 'staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml',
  wl_protocol_dir / 'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml',
  wl_protocol_dir / 'staging/ext-idle-notify/ext-idle-notify-v1.xml',
  wl_protocol_dir / 'unstable/xdg-output/xdg-output-unstable-v1.xml',
  wl_protocol_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
  wl_protocol_dir / 'unstable/tablet/tablet-unstable-v2.xml',
  'protocols/wlr-layer-shell-unstable-v1.xml',
  'protocols/wlr-foreign-toplevel-management-unstable-v1.xml',
]

proto_sources = []
proto_headers = []

foreach p : protocols
  proto_sources += wayland_scanner_code.process(p)
  proto_headers += wayland_scanner_client.process(p)
endforeach

proto_lib = static_library('waywal_proto',
  proto_sources,
  proto_headers,
  dependencies: [wayland_client_dep]
)

proto_dep = declare_dependency(
  link_with: proto_lib,
  sources: proto_headers,
  dependencies: [wayland_client_dep]
)

# Image dependencies for common loader
png_dep = dependency('libpng', required: false)
turbojpeg_dep = dependency('libturbojpeg', required: false)

# ==============================================================================
# WayWal Core Static Library (libwaywal_core.a)
# Shared cleanly between wwald (daemon), wwal (CLI), and wwal-studio (GUI)
# ==============================================================================

core_sources = files(
  'src/common/arena.c',
  'src/common/path.c',
  'src/common/log.c',
  'src/common/os_compat.c',
  'src/common/ipc.c',
  'src/common/ipc_proto.c',
  'src/common/bezier.c',
  'src/common/palette_hct.c',
  'src/common/toml_parser.c',
  'src/common/state_store.c',
  'src/common/image_loader.c',
  'src/common/we_pkg.c',
  'src/common/we_json.c',
  'src/common/we_project.c',
  'src/common/solar_schedule.c',
  'src/common/shader_parser.c',
  'src/common/we_vdf.c',
  'src/common/we_appworkshop.c',
  'src/common/wwpkg.c',
  'src/common/we_compat_scanner.c',
  'src/common/shader_graph.c',
)

core_deps = [threads_dep, m_dep, dl_dep]
if png_dep.found()
  core_deps += png_dep
endif
if turbojpeg_dep.found()
  core_deps += turbojpeg_dep
endif
if openmp_dep.found()
  core_deps += openmp_dep
endif

waywal_core_lib = static_library('waywal_core',
  core_sources,
  include_directories: inc_dir,
  dependencies: core_deps,
  pic: true,
  install: false,
)

waywal_core_dep = declare_dependency(
  link_with: waywal_core_lib,
  include_directories: inc_dir,
  dependencies: core_deps,
)

common_sources = core_sources
common_lib = waywal_core_lib
common_dep = waywal_core_dep


# Daemon executable (wwald)
daemon_sources = files(
  'src/daemon/main.c',
  'src/daemon/ipc_server.c',
  'src/daemon/wayland_core.c',
  'src/daemon/layer_shell.c',
  'src/daemon/dmabuf_alloc.c',
  'src/daemon/dmabuf_feedback.c',
  'src/daemon/render_engine.c',
  'src/daemon/gpu_compute.c',
  'src/daemon/simd/simd_blend.c',
  'src/daemon/simd/blend_scalar.c',
  'src/daemon/simd/blend_avx2.c',
  'src/daemon/simd/blend_avx512.c',
  'src/daemon/simd/blend_neon.c',
  'src/daemon/simd/cpu_transitions.c',
  'src/daemon/demuxer.c',
  'src/daemon/video_vaapi.c',
  'src/daemon/vaapi_h264.c',
  'src/daemon/vaapi_hevc.c',
  'src/daemon/vaapi_vp9.c',
  'src/daemon/vaapi_av1.c',
  'src/daemon/video_engine.c',
  'src/daemon/presentation.c',
  'src/daemon/uring_loop.c',
  'src/daemon/output_state.c',
  'src/daemon/security.c',
  'src/daemon/slideshow.c',
  'src/daemon/anim_engine.c',
  'src/daemon/power_governor.c',
  'src/daemon/audio_fft.c',
  'src/daemon/mpris.c',
  'src/daemon/shader_engine.c',
  'src/daemon/we_tex.c',
  'src/daemon/we_shader.c',
  'src/daemon/we_puppet.c',
  'src/daemon/we_particle.c',
  'src/daemon/we_script_parse.c',
  'src/daemon/we_script.c',
  'src/daemon/we_scene_parse.c',
  'src/daemon/we_scene.c',
  'src/daemon/we_builtin_effects.c',
  'src/daemon/we_physics.c',
  'src/daemon/we_camera3d.c',
)

daemon_deps = [
  waywal_core_dep,
  proto_dep,
  shaders_dep,
  wayland_client_dep,
  libdrm_dep,
  gbm_dep,
  egl_dep,
  glesv2_dep,
  vaapi_dep,
  vaapi_drm_dep,
  liburing_dep,
  seccomp_dep,
  threads_dep,
  m_dep
]
if have_ffmpeg_swdec
  daemon_deps += [
    avcodec_dep.partial_dependency(compile_args: true, includes: true),
    avutil_dep.partial_dependency(compile_args: true, includes: true),
    swscale_dep.partial_dependency(compile_args: true, includes: true),
  ]
endif
if openmp_dep.found()
  daemon_deps += openmp_dep
endif

wwald = executable('wwald',
  daemon_sources,
  include_directories: inc_dir,
  dependencies: daemon_deps,
  install: true
)

# Client executable (wwal)
client_sources = files(
  'src/client/main.c',
  'src/client/cli_parser.c',
  'src/client/gif_transcode.c',
)

client_deps = [waywal_core_dep, threads_dep, m_dep]

wwal = executable('wwal',
  client_sources,
  include_directories: inc_dir,
  dependencies: client_deps,
  install: true
)

if get_option('swww_compat')
  install_symlink('swww', pointing_to: 'wwal', install_dir: get_option('bindir'))
  install_symlink('awww', pointing_to: 'wwal', install_dir: get_option('bindir'))
  install_symlink('swww-daemon', pointing_to: 'wwald', install_dir: get_option('bindir'))
  install_symlink('awww-daemon', pointing_to: 'wwald', install_dir: get_option('bindir'))
endif

# ==============================================================================
# WayWal Studio GUI Application (wwal-studio)
# ==============================================================================

qt6_opt = get_option('studio')
have_qt6 = false
qt6_dep = dependency('qt6', modules: ['Core', 'Gui', 'Widgets', 'OpenGLWidgets'], required: qt6_opt)
sqlite3_dep = dependency('sqlite3', required: qt6_opt)
if qt6_dep.found() and sqlite3_dep.found()
  qt6 = import('qt6')
  have_qt6 = true

  studio_moc_headers = files(
    'src/studio/app/dock_layout_manager.hpp',
    'src/studio/app/main_window.hpp',
    'src/studio/core/command_bus.hpp',
    'src/studio/core/ipc_client.hpp',
    'src/studio/core/preset_store.hpp',
    'src/studio/dock/openhub_browser_widget.hpp',
    'src/studio/dock/workshop_browser_widget.hpp',
    'src/studio/footer/audio_spectrum_widget.hpp',
    'src/studio/footer/power_governor_widget.hpp',
    'src/studio/footer/studio_footer_bar.hpp',
    'src/studio/footer/telemetry_hud_widget.hpp',
    'src/studio/grid/hover_preview_controller.hpp',
    'src/studio/grid/virtualized_grid_widget.hpp',
    'src/studio/inspector/oklch_color_dialog.hpp',
    'src/studio/inspector/property_inspector_widget.hpp',
    'src/studio/theme/desktop_preview_mockup.hpp',
    'src/studio/theme/template_exporter.hpp',
    'src/studio/theme/theme_studio_widget.hpp',
    'src/studio/topology/cubic_bezier_editor.hpp',
    'src/studio/topology/monitor_topology_canvas.hpp',
    'src/studio/viewport/gpu_preview_viewport.hpp',
    'src/studio/wizard/creator_packaging_wizard.hpp',
    'src/studio/graph/node_graph_editor_widget.hpp',
    'src/studio/graph/effect_library_browser_widget.hpp',
    'src/studio/inspector/camera_lighting_inspector_widget.hpp',
    'src/studio/inspector/physics_spring_inspector_widget.hpp',
  )

  studio_inc_dirs = [inc_dir, include_directories('src', 'src/studio')]

  studio_mocs = qt6.compile_moc(
    headers: studio_moc_headers,
    include_directories: studio_inc_dirs,
    dependencies: qt6_dep,
  )

  studio_lib_sources = files(
    'src/studio/app/dock_layout_manager.cpp',
    'src/studio/app/main_window.cpp',
    'src/studio/core/catalog_index_manager.cpp',
    'src/studio/core/command_bus.cpp',
    'src/studio/core/ipc_client.cpp',
    'src/studio/core/preset_store.cpp',
    'src/studio/core/recursive_fs_watcher.cpp',
    'src/studio/dock/openhub_browser_widget.cpp',
    'src/studio/dock/workshop_browser_widget.cpp',
    'src/studio/footer/audio_spectrum_widget.cpp',
    'src/studio/footer/power_governor_widget.cpp',
    'src/studio/footer/studio_footer_bar.cpp',
    'src/studio/footer/telemetry_hud_widget.cpp',
    'src/studio/grid/fuzzy_catalog_index.cpp',
    'src/studio/grid/gpu_texture_pool.cpp',
    'src/studio/grid/hover_preview_controller.cpp',
    'src/studio/grid/virtualized_grid_widget.cpp',
    'src/studio/inspector/oklch_color_dialog.cpp',
    'src/studio/inspector/property_inspector_widget.cpp',
    'src/studio/queue/download_manager.cpp',
    'src/studio/theme/desktop_preview_mockup.cpp',
    'src/studio/theme/template_exporter.cpp',
    'src/studio/theme/theme_studio_widget.cpp',
    'src/studio/topology/cubic_bezier_editor.cpp',
    'src/studio/topology/monitor_topology_canvas.cpp',
    'src/studio/viewport/gpu_preview_viewport.cpp',
    'src/studio/wizard/creator_packaging_wizard.cpp',
    'src/studio/graph/node_graph_editor_widget.cpp',
    'src/studio/graph/effect_library_browser_widget.cpp',
    'src/studio/inspector/camera_lighting_inspector_widget.cpp',
    'src/studio/inspector/physics_spring_inspector_widget.cpp',
    'src/daemon/we_builtin_effects.c',
    'src/daemon/we_physics.c',
    'src/daemon/we_camera3d.c',
  ) + studio_mocs

  studio_deps = [
    qt6_dep,
    waywal_core_dep,
    sqlite3_dep,
    threads_dep,
    m_dep,
    dl_dep,
    egl_dep,
    glesv2_dep,
    libdrm_dep,
  ]

  waywal_studio_lib = static_library('waywal_studio',
    studio_lib_sources,
    include_directories: studio_inc_dirs,
    dependencies: studio_deps,
    pic: true,
    install: false,
  )

  waywal_studio_dep = declare_dependency(
    link_with: waywal_studio_lib,
    include_directories: studio_inc_dirs,
    dependencies: studio_deps,
  )

  wwal_studio = executable('wwal-studio',
    'src/studio/main.cpp',
    include_directories: studio_inc_dirs,
    dependencies: [waywal_studio_dep],
    install: true,
  )
endif

# Systemd integration
if get_option('enable_systemd')
  systemd_dep = dependency('systemd', required: false)
  if systemd_dep.found()
    systemd_user_unit_dir = systemd_dep.get_variable(
      pkgconfig: 'systemduserunitdir',
      pkgconfig_define: ['prefix', get_option('prefix')],
      default_value: get_option('prefix') / 'lib/systemd/user'
    )
  else
    systemd_user_unit_dir = get_option('prefix') / 'lib/systemd/user'
  endif
  install_data('contrib/systemd/wwald.service', install_dir: systemd_user_unit_dir)
  install_data('contrib/systemd/wwald.socket', install_dir: systemd_user_unit_dir)
endif

# Manual pages
man_pages_opt = get_option('man-pages')
if not man_pages_opt.disabled()
  scdoc = find_program('scdoc', required: man_pages_opt.enabled())
  if scdoc.found()
    foreach m : [['wwal.1', 'man1'], ['wwald.1', 'man1'], ['wwal-config.5', 'man5']]
      custom_target(m[0],
        input: 'docs/man/' + m[0] + '.scd',
        output: m[0],
        command: [scdoc],
        feed: true,
        capture: true,
        install: true,
        install_dir: get_option('mandir') / m[1]
      )
    endforeach
  else
    install_man(
      'docs/man/wwal.1',
      'docs/man/wwald.1',
      'docs/man/wwal-config.5'
    )
  endif
endif

# Shell completions
install_data('contrib/completions/wwal.bash', install_dir: get_option('datadir') / 'bash-completion/completions', rename: 'wwal')
install_data('contrib/completions/wwal.zsh', install_dir: get_option('datadir') / 'zsh/site-functions', rename: '_wwal')
install_data('contrib/completions/wwal.fish', install_dir: get_option('datadir') / 'fish/vendor_completions.d')

# Sample & preset shaders and example configuration
install_data(
  'contrib/shaders/plasma.frag',
  'contrib/shaders/audio_spectrum.frag',
  install_dir: get_option('datadir') / 'wwal/shaders'
)
fs = import('fs')
if fs.is_dir('shaders/presets')
  install_subdir('shaders/presets', install_dir: get_option('datadir') / 'wwal/shaders')
elif fs.is_dir('contrib/shaders/presets')
  install_subdir('contrib/shaders/presets', install_dir: get_option('datadir') / 'wwal/shaders')
endif
install_data('contrib/config.toml.example', install_dir: get_option('datadir') / 'doc/wwal')


# Test & Benchmark Suites
if get_option('tests')
  test_core_exe = executable('test_core',
    'tests/test_core.c',
    include_directories: inc_dir,
    dependencies: [common_dep]
  )

  test_dmabuf_exe = executable('test_dmabuf',
    'tests/test_dmabuf.c',
    'src/daemon/dmabuf_alloc.c',
    'src/daemon/dmabuf_feedback.c',
    include_directories: inc_dir,
    dependencies: [
      common_dep,
      proto_dep,
      wayland_client_dep,
      libdrm_dep,
      gbm_dep
    ]
  )

  test_transitions_exe = executable('test_transitions',
    'tests/test_transitions.c',
    'src/daemon/layer_shell.c',
    'src/daemon/presentation.c',
    'src/daemon/uring_loop.c',
    'src/daemon/render_engine.c',
    'src/daemon/gpu_compute.c',
    'src/daemon/shader_engine.c',
    'src/daemon/dmabuf_alloc.c',
    'src/daemon/dmabuf_feedback.c',
    'src/daemon/simd/simd_blend.c',
    'src/daemon/simd/blend_scalar.c',
    'src/daemon/simd/blend_avx2.c',
    'src/daemon/simd/blend_avx512.c',
    'src/daemon/simd/blend_neon.c',
    'src/daemon/simd/cpu_transitions.c',
    include_directories: [inc_dir, include_directories('src/daemon')],
    dependencies: [
      common_dep,
      proto_dep,
      shaders_dep,
      wayland_client_dep,
      libdrm_dep,
      gbm_dep,
      egl_dep,
      glesv2_dep,
      liburing_dep,
      threads_dep,
      m_dep
    ] + (openmp_dep.found() ? [openmp_dep] : [])
  )

  bench_transitions_exe = executable('bench_transitions',
    'tests/bench_transitions.c',
    'src/daemon/render_engine.c',
    'src/daemon/gpu_compute.c',
    'src/daemon/dmabuf_alloc.c',
    'src/daemon/dmabuf_feedback.c',
    'src/daemon/simd/simd_blend.c',
    'src/daemon/simd/blend_scalar.c',
    'src/daemon/simd/blend_avx2.c',
    'src/daemon/simd/blend_avx512.c',
    'src/daemon/simd/blend_neon.c',
    'src/daemon/simd/cpu_transitions.c',
    include_directories: inc_dir,
    dependencies: [
      common_dep,
      proto_dep,
      shaders_dep,
      wayland_client_dep,
      libdrm_dep,
      gbm_dep,
      egl_dep,
      glesv2_dep,
      threads_dep,
      m_dep
    ] + (openmp_dep.found() ? [openmp_dep] : [])
  )

  test_video_exe = executable('test_video',
    'tests/test_video.c',
    'src/daemon/demuxer.c',
    'src/daemon/video_vaapi.c',
    'src/daemon/vaapi_h264.c',
    'src/daemon/vaapi_hevc.c',
    'src/daemon/vaapi_vp9.c',
    'src/daemon/vaapi_av1.c',
    'src/daemon/dmabuf_alloc.c',
    'src/daemon/dmabuf_feedback.c',
    include_directories: inc_dir,
    dependencies: [
      common_dep,
      proto_dep,
      wayland_client_dep,
      libdrm_dep,
      gbm_dep,
      egl_dep,
      glesv2_dep,
      vaapi_dep,
      vaapi_drm_dep,
      threads_dep,
      m_dep
    ]
  )

  test_desktop_exe = executable('test_desktop',
    'tests/test_desktop.c',
    'src/daemon/presentation.c',
    'src/daemon/uring_loop.c',
    'src/daemon/output_state.c',
    'src/daemon/security.c',
    'src/daemon/dmabuf_alloc.c',
    'src/daemon/dmabuf_feedback.c',
    include_directories: inc_dir,
    dependencies: [
      common_dep,
      proto_dep,
      wayland_client_dep,
      libdrm_dep,
      gbm_dep,
      liburing_dep,
      seccomp_dep,
      threads_dep,
      m_dep
    ]
  )

  test('core', test_core_exe)
  test('dmabuf', test_dmabuf_exe)
  test('transitions', test_transitions_exe)
  test('video', test_video_exe)
  test('desktop', test_desktop_exe)

  advanced_test_deps = [common_dep, proto_dep, wayland_client_dep, libdrm_dep, gbm_dep, threads_dep, m_dep]

  test_advanced_exe = executable('test_advanced',
    'tests/test_advanced.c',
    'src/client/cli_parser.c',
    'src/client/gif_transcode.c',
    'src/daemon/slideshow.c',
    include_directories: inc_dir,
    dependencies: advanced_test_deps
  )

  test('advanced', test_advanced_exe)

  test('integration', find_program('tests/integration_test.sh'),
    depends: [wwald, wwal],
    timeout: 120
  )

  # E2E Test Suite (Tiers 1-4)
  test_e2e_sources = files(
    'tests/e2e/test_e2e_runner.c',
    'tests/e2e/tier1_features.c',
    'tests/e2e/tier2_boundaries.c',
    'tests/e2e/tier3_combinations.c',
    'tests/e2e/tier4_workloads.c',
    'src/daemon/render_engine.c',
    'src/daemon/gpu_compute.c',
    'src/daemon/dmabuf_alloc.c',
    'src/daemon/dmabuf_feedback.c',
    'src/daemon/simd/simd_blend.c',
    'src/daemon/simd/blend_scalar.c',
    'src/daemon/simd/blend_avx2.c',
    'src/daemon/simd/blend_avx512.c',
    'src/daemon/simd/blend_neon.c',
    'src/daemon/simd/cpu_transitions.c',
  )

  test_e2e_exe = executable('test_e2e',
    test_e2e_sources,
    include_directories: inc_dir,
    dependencies: [
      common_dep,
      proto_dep,
      shaders_dep,
      wayland_client_dep,
      libdrm_dep,
      gbm_dep,
      egl_dep,
      glesv2_dep,
      threads_dep,
      m_dep
    ] + (openmp_dep.found() ? [openmp_dep] : [])
  )

  test('e2e', test_e2e_exe, timeout: 180)

  test_subsystems_deps = [
    common_dep,
    proto_dep,
    shaders_dep,
    wayland_client_dep,
    libdrm_dep,
    gbm_dep,
    egl_dep,
    glesv2_dep,
    threads_dep,
    m_dep,
    dl_dep
  ]
  if have_ffmpeg_swdec
    test_subsystems_deps += [
      avcodec_dep.partial_dependency(compile_args: true, includes: true),
      avutil_dep.partial_dependency(compile_args: true, includes: true),
      swscale_dep.partial_dependency(compile_args: true, includes: true),
    ]
  endif
  if openmp_dep.found()
    test_subsystems_deps += openmp_dep
  endif

  test_subsystems_exe = executable('test_subsystems',
    'tests/test_subsystems.c',
    'src/client/cli_parser.c',
    'src/daemon/anim_engine.c',
    'src/daemon/power_governor.c',
    'src/daemon/audio_fft.c',
    'src/daemon/mpris.c',
    'src/daemon/shader_engine.c',
    'src/daemon/slideshow.c',
    'src/daemon/gpu_compute.c',
    'src/daemon/dmabuf_alloc.c',
    'src/daemon/dmabuf_feedback.c',
    include_directories: [inc_dir, include_directories('src/daemon')],
    dependencies: test_subsystems_deps
  )

  test('subsystems', test_subsystems_exe)

  test_we_core_exe = executable('test_we_core',
    'tests/test_we_core.c',
    'src/client/cli_parser.c',
    'src/daemon/demuxer.c',
    'src/daemon/audio_fft.c',
    'src/daemon/mpris.c',
    'src/daemon/shader_engine.c',
    'src/daemon/gpu_compute.c',
    'src/daemon/we_tex.c',
    'src/daemon/we_shader.c',
    'src/daemon/we_puppet.c',
    'src/daemon/we_particle.c',
    'src/daemon/we_script_parse.c',
    'src/daemon/we_script.c',
    'src/daemon/we_scene_parse.c',
    'src/daemon/we_scene.c',
    'src/daemon/we_builtin_effects.c',
    'src/daemon/we_physics.c',
    'src/daemon/we_camera3d.c',
    'src/daemon/dmabuf_alloc.c',
    'src/daemon/dmabuf_feedback.c',
    include_directories: [inc_dir, include_directories('src/daemon')],
    dependencies: test_subsystems_deps
  )

  test('we_core', test_we_core_exe, depends: [wwal, wwald], timeout: 120)

  fuzz_demuxer_exe = executable('fuzz_demuxer',
    'tests/fuzz/fuzz_demuxer.c',
    'src/daemon/demuxer.c',
    c_args: ['-DWWAL_FUZZ_STANDALONE'],
    include_directories: inc_dir,
    dependencies: [common_dep, m_dep]
  )

  fuzz_bitstream_exe = executable('fuzz_bitstream',
    'tests/fuzz/fuzz_bitstream.c',
    'src/daemon/video_vaapi.c',
    'src/daemon/vaapi_h264.c',
    'src/daemon/vaapi_hevc.c',
    'src/daemon/vaapi_vp9.c',
    'src/daemon/vaapi_av1.c',
    'src/daemon/dmabuf_alloc.c',
    'src/daemon/dmabuf_feedback.c',
    c_args: ['-DWWAL_FUZZ_STANDALONE'],
    include_directories: inc_dir,
    dependencies: [
      common_dep,
      proto_dep,
      wayland_client_dep,
      libdrm_dep,
      gbm_dep,
      vaapi_dep,
      vaapi_drm_dep,
      threads_dep,
      m_dep
    ]
  )

  fuzz_config_cli_exe = executable('fuzz_config_cli',
    'tests/fuzz/fuzz_config_cli.c',
    'src/client/cli_parser.c',
    c_args: ['-DWWAL_FUZZ_STANDALONE'],
    include_directories: inc_dir,
    dependencies: [common_dep, m_dep]
  )

  test('fuzz_demuxer', fuzz_demuxer_exe)
  test('fuzz_bitstream', fuzz_bitstream_exe)
  test('fuzz_config_cli', fuzz_config_cli_exe)

  # Core C++23 Compatibility, IPC Invariant & Studio Test Suites
  test_core_cxx23_compat_exe = executable('test_core_cxx23_compat',
    'tests/test_core_cxx23_compat.cpp',
    include_directories: [inc_dir, include_directories('src')],
    dependencies: [waywal_core_dep, proto_dep, libdrm_dep]
  )
  test('core_cxx23_compat', test_core_cxx23_compat_exe)

  test_ipc_legacy_72byte_exe = executable('test_ipc_legacy_72byte',
    'tests/test_ipc_legacy_72byte.c',
    include_directories: inc_dir,
    dependencies: [waywal_core_dep]
  )
  test('ipc_legacy_72byte', test_ipc_legacy_72byte_exe)

  # Workshop, Open Hub & Compatibility Test Targets
  test_we_vdf_exe = executable('test_we_vdf',
    'tests/test_we_vdf.c',
    include_directories: inc_dir,
    dependencies: [waywal_core_dep, m_dep]
  )
  test('we_vdf', test_we_vdf_exe, is_parallel: false)

  test_wwpkg_crypto_exe = executable('test_wwpkg_crypto',
    'tests/test_wwpkg_crypto.c',
    include_directories: inc_dir,
    dependencies: [waywal_core_dep, m_dep, dl_dep]
  )
  test('wwpkg_crypto', test_wwpkg_crypto_exe, timeout: 60)

  test_we_compat_scanner_exe = executable('test_we_compat_scanner',
    'tests/test_we_compat_scanner.c',
    include_directories: inc_dir,
    dependencies: [waywal_core_dep, m_dep]
  )
  test('we_compat_scanner', test_we_compat_scanner_exe, timeout: 60)

  fuzz_workshop_parsers_exe = executable('fuzz_workshop_parsers',
    'tests/fuzz/fuzz_workshop_parsers.c',
    c_args: ['-DWWAL_FUZZ_STANDALONE'],
    include_directories: inc_dir,
    dependencies: [waywal_core_dep, m_dep, dl_dep]
  )
  test('fuzz_workshop_parsers', fuzz_workshop_parsers_exe, timeout: 60)

  test_workshop_security_exe = executable('test_workshop_security',
    'tests/test_workshop_security.c',
    include_directories: inc_dir,
    dependencies: [waywal_core_dep, m_dep, dl_dep]
  )
  test('workshop_security', test_workshop_security_exe, timeout: 60)

  # Built-in Effects, Physics, 3D Camera & Shader Graph Test Suites
  test_we_builtin_effects_exe = executable('test_we_builtin_effects',
    'tests/test_we_builtin_effects.c',
    'src/daemon/we_builtin_effects.c',
    'src/daemon/we_shader.c',
    include_directories: [inc_dir, include_directories('src/daemon')],
    dependencies: [waywal_core_dep, egl_dep, glesv2_dep, gbm_dep, libdrm_dep, m_dep]
  )
  test('we_builtin_effects', test_we_builtin_effects_exe)

  test_we_physics_exe = executable('test_we_physics',
    'tests/test_we_physics.c',
    'src/daemon/we_physics.c',
    include_directories: [inc_dir, include_directories('src/daemon')],
    dependencies: [waywal_core_dep, m_dep]
  )
  test('we_physics', test_we_physics_exe)

  test_we_camera3d_exe = executable('test_we_camera3d',
    'tests/test_we_camera3d.c',
    'src/daemon/we_camera3d.c',
    include_directories: [inc_dir, include_directories('src/daemon')],
    dependencies: [waywal_core_dep, m_dep]
  )
  test('we_camera3d', test_we_camera3d_exe)

  test_shader_graph_compiler_exe = executable('test_shader_graph_compiler',
    'tests/test_shader_graph_compiler.c',
    include_directories: [inc_dir, include_directories('src/daemon')],
    dependencies: [waywal_core_dep, m_dep]
  )
  test('shader_graph_compiler', test_shader_graph_compiler_exe)

  test_ipc_phase3_wire_exe = executable('test_ipc_phase3_wire',
    'tests/test_ipc_phase3_wire.c',
    include_directories: inc_dir,
    dependencies: [waywal_core_dep, m_dep]
  )
  test('ipc_phase3_wire', test_ipc_phase3_wire_exe)

  test_challenger_effects_exe = executable('test_challenger_effects',
    'tests/test_challenger_effects.c',
    'src/daemon/we_builtin_effects.c',
    include_directories: [inc_dir, include_directories('src/daemon')],
    dependencies: [waywal_core_dep, m_dep]
  )
  test('challenger_effects', test_challenger_effects_exe)

  test_challenger_physics_exe = executable('test_challenger_physics',
    'tests/test_challenger_physics.c',
    'src/daemon/we_physics.c',
    include_directories: [inc_dir, include_directories('src/daemon')],
    dependencies: [waywal_core_dep, m_dep]
  )
  test('challenger_physics', test_challenger_physics_exe)

  test_challenger_camera3d_exe = executable('test_challenger_camera3d',
    'tests/test_challenger_camera3d.c',
    'src/daemon/we_camera3d.c',
    include_directories: [inc_dir, include_directories('src/daemon')],
    dependencies: [waywal_core_dep, m_dep]
  )
  test('challenger_camera3d', test_challenger_camera3d_exe)

  test_challenger_shader_graph_exe = executable('test_challenger_shader_graph',
    'tests/test_challenger_shader_graph.c',
    include_directories: [inc_dir, include_directories('src/daemon')],
    dependencies: [waywal_core_dep, m_dep]
  )
  test('challenger_shader_graph', test_challenger_shader_graph_exe)

  if have_qt6
    test_studio_command_and_dock_exe = executable('test_studio_command_and_dock',
      'tests/test_studio_command_and_dock.cpp',
      include_directories: studio_inc_dirs,
      dependencies: [waywal_studio_dep],
    )
    test('studio_command_and_dock', test_studio_command_and_dock_exe)

    test_studio_phase3_integration_exe = executable('test_studio_phase3_integration',
      'tests/test_studio_phase3_integration.cpp',
      include_directories: studio_inc_dirs,
      dependencies: [waywal_studio_dep],
    )
    test('studio_phase3_integration', test_studio_phase3_integration_exe, timeout: 60, is_parallel: false)

    benchmark_grid_virtualization_exe = executable('benchmark_grid_virtualization',
      'tests/benchmark_grid_virtualization.cpp',
      include_directories: studio_inc_dirs,
      dependencies: [waywal_studio_dep],
    )
    test('benchmark_grid_virtualization', benchmark_grid_virtualization_exe, timeout: 60, is_parallel: false)

    test_catalog_index_exe = executable('test_catalog_index',
      'tests/test_catalog_index.cpp',
      include_directories: studio_inc_dirs,
      dependencies: [waywal_studio_dep],
    )
    test('catalog_index', test_catalog_index_exe, timeout: 60, is_parallel: false, priority: 50)

    test_recursive_fs_watcher_exe = executable('test_recursive_fs_watcher',
      'tests/test_recursive_fs_watcher.cpp',
      include_directories: studio_inc_dirs,
      dependencies: [waywal_studio_dep],
    )
    test('recursive_fs_watcher', test_recursive_fs_watcher_exe, timeout: 30)

    test_download_manager_exe = executable('test_download_manager',
      'tests/test_download_manager.cpp',
      include_directories: studio_inc_dirs,
      dependencies: [waywal_studio_dep],
    )
    test('download_manager', test_download_manager_exe, timeout: 30)
  endif
endif

summary({
  'C compiler': cc.get_id() + ' ' + cc.version(),
  'C++ compiler': cpp.get_id() + ' ' + cpp.version(),
  'Install prefix': get_option('prefix'),
  'Systemd units': get_option('enable_systemd'),
  'Manual pages': not man_pages_opt.disabled(),
  'Test suites': get_option('tests'),
}, section: 'Build environment', bool_yn: true)

summary({
  'wwal-studio (Qt6 GUI)': have_qt6,
  'VA-API hardware video': vaapi_dep.found() and vaapi_drm_dep.found(),
  'io_uring event loop': liburing_dep.found(),
  'Seccomp-BPF sandbox': seccomp_dep.found(),
  'OpenMP multi-threading': openmp_dep.found(),
  'SIMD vector kernels': get_option('enable_simd'),
  'libpng native loader': png_dep.found(),
  'libturbojpeg loader': turbojpeg_dep.found(),
  'WebP (dlopen)': not get_option('webp').disabled(),
  'AVIF (dlopen)': not get_option('avif').disabled(),
  'JPEG-XL (dlopen)': not get_option('jxl').disabled(),
  'SVG / librsvg (dlopen)': not get_option('svg').disabled(),
  'PipeWire FFT (dlopen)': not get_option('pipewire').disabled(),
  'FFmpeg SW Dec': have_ffmpeg_swdec,
}, section: 'Features & Codecs', bool_yn: true)

