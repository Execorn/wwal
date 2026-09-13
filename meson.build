project('waywal', 'c',
  version: '1.0.0',
  default_options: [
    'c_std=gnu2x',
    'optimization=3',
    'b_lto=true',
    'b_pie=true',
    'warning_level=3',
    'werror=false'
  ]
)

cc = meson.get_compiler('c')

# Optimization & Compiler arguments
add_project_arguments(
  '-fno-plt',
  '-fvisibility=hidden',
  '-fstrict-aliasing',
  '-D_GNU_SOURCE',
  language: 'c'
)

# Core dependencies
wayland_client_dep = dependency('wayland-client', version: '>=1.20')
wayland_protos_dep = dependency('wayland-protocols', version: '>=1.24')
wayland_scanner = find_program('wayland-scanner')
libdrm_dep = dependency('libdrm', version: '>=2.4.110')
gbm_dep = dependency('gbm')
egl_dep = dependency('egl')
glesv2_dep = dependency('glesv2')
threads_dep = dependency('threads')
m_dep = cc.find_library('m', required: false)

# Optional accelerated dependencies
vaapi_dep = dependency('libva', required: get_option('enable_vaapi'))
vaapi_drm_dep = dependency('libva-drm', required: get_option('enable_vaapi'))
liburing_dep = dependency('liburing', required: get_option('enable_io_uring'))
seccomp_dep = dependency('libseccomp', required: get_option('enable_seccomp'))
if seccomp_dep.found()
  add_project_arguments('-DHAVE_LIBSECCOMP', language: 'c')
endif

inc_dir = include_directories('include', 'src/client')

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

proto_sources = []
proto_headers = []

protocols = [
  'protocols/xdg-shell.xml',
  'protocols/wlr-layer-shell-unstable-v1.xml',
  'protocols/viewporter.xml',
  'protocols/fractional-scale-v1.xml',
  'protocols/presentation-time.xml',
  'protocols/linux-dmabuf-v1.xml',
]

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

# Image dependencies for client
png_dep = dependency('libpng', required: false)
turbojpeg_dep = dependency('libturbojpeg', required: false)

# Common foundation library
common_sources = files(
  'src/common/arena.c',
  'src/common/path.c',
  'src/common/log.c',
  'src/common/os_compat.c',
  'src/common/ipc_proto.c',
  'src/common/bezier.c',
)

common_lib = static_library('waywal_common',
  common_sources,
  include_directories: inc_dir,
  dependencies: [threads_dep, m_dep]
)

common_dep = declare_dependency(
  link_with: common_lib,
  include_directories: inc_dir,
  dependencies: [threads_dep, m_dep]
)

# Daemon executable (wwald)
daemon_sources = files(
  'src/daemon/main.c',
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
  'src/daemon/demuxer.c',
  'src/daemon/video_vaapi.c',
  'src/daemon/video_engine.c',
  'src/daemon/presentation.c',
  'src/daemon/uring_loop.c',
  'src/daemon/output_state.c',
  'src/daemon/security.c',
  'src/daemon/slideshow.c',
  'src/client/image_loader.c',
)

daemon_deps = [
  common_dep,
  proto_dep,
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
if png_dep.found()
  daemon_deps += png_dep
endif
if turbojpeg_dep.found()
  daemon_deps += turbojpeg_dep
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
  'src/client/image_loader.c',
)

client_deps = [common_dep, threads_dep, m_dep]
if png_dep.found()
  client_deps += png_dep
endif
if turbojpeg_dep.found()
  client_deps += turbojpeg_dep
endif

wwal = executable('wwal',
  client_sources,
  include_directories: inc_dir,
  dependencies: client_deps,
  install: true
)

# Systemd integration
if get_option('enable_systemd')
  systemd_dep = dependency('systemd', required: false)
  if systemd_dep.found()
    systemd_user_unit_dir = systemd_dep.get_variable(pkgconfig: 'systemduserunitdir', default_value: get_option('prefix') / 'lib/systemd/user')
  else
    systemd_user_unit_dir = get_option('prefix') / 'lib/systemd/user'
  endif
  install_data('contrib/systemd/wwald.service', install_dir: systemd_user_unit_dir)
endif

# Shell completions
install_data('contrib/completions/wwal.bash', install_dir: get_option('datadir') / 'bash-completion/completions', rename: 'wwal')
install_data('contrib/completions/wwal.zsh', install_dir: get_option('datadir') / 'zsh/site-functions', rename: '_wwal')
install_data('contrib/completions/wwal.fish', install_dir: get_option('datadir') / 'fish/vendor_completions.d')

# Unit Tests
test_phase1_exe = executable('test_phase1',
  'tests/test_phase1.c',
  include_directories: inc_dir,
  dependencies: [common_dep]
)

test_phase2_exe = executable('test_phase2',
  'tests/test_phase2.c',
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

test_phase3_exe = executable('test_phase3',
  'tests/test_phase3.c',
  'src/daemon/render_engine.c',
  'src/daemon/gpu_compute.c',
  'src/daemon/dmabuf_alloc.c',
  'src/daemon/dmabuf_feedback.c',
  'src/daemon/simd/simd_blend.c',
  'src/daemon/simd/blend_scalar.c',
  'src/daemon/simd/blend_avx2.c',
  'src/daemon/simd/blend_avx512.c',
  'src/daemon/simd/blend_neon.c',
  include_directories: inc_dir,
  dependencies: [
    common_dep,
    proto_dep,
    wayland_client_dep,
    libdrm_dep,
    gbm_dep,
    egl_dep,
    glesv2_dep,
    threads_dep,
    m_dep
  ]
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
  include_directories: inc_dir,
  dependencies: [
    common_dep,
    proto_dep,
    wayland_client_dep,
    libdrm_dep,
    gbm_dep,
    egl_dep,
    glesv2_dep,
    threads_dep,
    m_dep
  ]
)

test_phase4_exe = executable('test_phase4',
  'tests/test_phase4.c',
  'src/daemon/demuxer.c',
  'src/daemon/video_vaapi.c',
  'src/daemon/video_engine.c',
  'src/daemon/dmabuf_alloc.c',
  'src/daemon/dmabuf_feedback.c',
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

test('phase1', test_phase1_exe)
test('phase2', test_phase2_exe)
test('phase3', test_phase3_exe)
test('phase4', test_phase4_exe)

test_phase5_exe = executable('test_phase5',
  'tests/test_phase5.c',
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

test('phase5', test_phase5_exe)

advanced_test_deps = [common_dep, proto_dep, wayland_client_dep, libdrm_dep, gbm_dep, threads_dep, m_dep]
if png_dep.found()
  advanced_test_deps += png_dep
endif
if turbojpeg_dep.found()
  advanced_test_deps += turbojpeg_dep
endif

test_advanced_exe = executable('test_advanced',
  'tests/test_advanced.c',
  'src/client/cli_parser.c',
  'src/client/image_loader.c',
  'src/daemon/slideshow.c',
  include_directories: inc_dir,
  dependencies: advanced_test_deps
)

test('advanced', test_advanced_exe)

test('integration', find_program('tests/integration_test.sh'),
  depends: [wwald, wwal]
)
