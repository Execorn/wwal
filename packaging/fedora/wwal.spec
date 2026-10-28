Name:           wwal
Version:        1.1.0
Release:        1%{?dist}
Summary:        Zero-copy Wayland wallpaper, VA-API video, Shadertoy, and Material You engine

License:        GPL-3.0-or-later
URL:            https://github.com/Execorn/wwal
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc >= 13
BuildRequires:  meson >= 0.61.0
BuildRequires:  ninja-build
BuildRequires:  pkgconfig(wayland-client) >= 1.20
BuildRequires:  pkgconfig(wayland-protocols) >= 1.34
BuildRequires:  wayland-scanner
BuildRequires:  pkgconfig(libdrm) >= 2.4.110
BuildRequires:  pkgconfig(gbm)
BuildRequires:  pkgconfig(egl)
BuildRequires:  pkgconfig(glesv2)
BuildRequires:  pkgconfig(libva)
BuildRequires:  pkgconfig(libva-drm)
BuildRequires:  pkgconfig(liburing)
BuildRequires:  pkgconfig(libseccomp)
BuildRequires:  pkgconfig(libpng)
BuildRequires:  pkgconfig(libturbojpeg)
BuildRequires:  pkgconfig(systemd)
BuildRequires:  scdoc
BuildRequires:  python3

Recommends:     libwebp
Recommends:     libavif
Recommends:     libjxl
Recommends:     librsvg2
Recommends:     pipewire-libs
Recommends:     dbus-libs
Recommends:     lz4-libs
Recommends:     ffmpeg-free
Recommends:     (rofi or fuzzel)

Provides:       swww = %{version}-%{release}
Provides:       awww = %{version}-%{release}
Conflicts:      swww

%description
WayWal (wwal / wwald) is a zero-copy C23 Wayland wallpaper, hardware VA-API
video, multi-pass Shadertoy, Steam Wallpaper Engine (431960) Workshop runtime,
and sub-millisecond OKLab/CAM16/HCT Material Design 3 color theme engine.
Includes drop-in swww and awww CLI compatibility symlinks and built-in
procedural shader presets.

%prep
%autosetup -n %{name}-%{version}

%build
%meson \
    -Dtests=false \
    -Denable_vaapi=true \
    -Denable_io_uring=true \
    -Denable_simd=true \
    -Denable_seccomp=true \
    -Denable_systemd=true \
    -Dswww_compat=true
%meson_build

%install
%meson_install

%files
%license LICENSE
%doc README.md CHANGELOG.md
%{_bindir}/wwal
%{_bindir}/wwald
%{_bindir}/swww
%{_bindir}/awww
%{_bindir}/swww-daemon
%{_bindir}/awww-daemon
%{_userunitdir}/wwald.service
%{_userunitdir}/wwald.socket
%{_mandir}/man1/wwal.1*
%{_mandir}/man1/wwald.1*
%{_mandir}/man5/wwal-config.5*
%{_datadir}/bash-completion/completions/wwal
%{_datadir}/zsh/site-functions/_wwal
%{_datadir}/fish/vendor_completions.d/wwal.fish
%{_datadir}/wwal/
%{_datadir}/doc/wwal/config.toml.example

%changelog
* Wed Oct 28 2026 Biryukov Nikita <n.biryukov.04@gmail.com> - 1.1.0-1
- Release 1.1.0: swww/awww v0.12.1 drop-in compatibility, unified wwal pick,
  dual-mode Material 3 + Matugen/Wallust/Pywal template engine, solar/schedule
  dynamic wallpapers, Workshop sound/text layers, and built-in shader presets.
