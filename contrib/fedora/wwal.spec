Name:           wwal
Version:        1.0.0
Release:        1%{?dist}
Summary:        Zero-copy Wayland wallpaper, VA-API video, Shadertoy, and Material You engine
License:        GPL-3.0-or-later
URL:            https://github.com/Execorn/wwal
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc >= 13
BuildRequires:  meson >= 0.60
BuildRequires:  ninja-build
BuildRequires:  pkgconfig(wayland-client) >= 1.20
BuildRequires:  pkgconfig(wayland-protocols) >= 1.24
BuildRequires:  pkgconfig(wayland-scanner)
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
BuildRequires:  systemd-rpm-macros

Recommends:     libwebp
Recommends:     libavif
Recommends:     libjxl
Recommends:     librsvg2
Recommends:     pipewire-libs

%description
wwal (WayWal) is a zero-copy C23 Wayland wallpaper engine combining static
DMA-BUF images, 18 GLES 3.1 compute shader transitions, zero-FFmpeg VA-API
hardware video playback, live Shadertoy shaders with PipeWire 64-bin Bark FFT
audio reactivity, Zero-Watt smart occlusion/battery governance, and sub-ms
OKLab/CAM16/HCT Material Design 3 palette extraction.

%package swww-compat
Summary:        Drop-in swww and awww CLI compatibility symlinks for wwal
Requires:       %{name} = %{version}-%{release}
Conflicts:      swww
Provides:       swww = %{version}-%{release}

%description swww-compat
Provides drop-in swww, swww-daemon, awww, and awww-daemon compatibility
symlinks backed by wwal and wwald.

%prep
%autosetup -n %{name}-%{version}

%build
%meson \
    -Denable_vaapi=true \
    -Denable_io_uring=true \
    -Denable_simd=true \
    -Denable_seccomp=true \
    -Denable_systemd=true
%meson_build

%install
%meson_install
install -Dpm0644 contrib/systemd/wwald.socket %{buildroot}%{_userunitdir}/wwald.socket
install -Dpm0644 docs/man/wwal.1 %{buildroot}%{_mandir}/man1/wwal.1
install -Dpm0644 docs/man/wwald.1 %{buildroot}%{_mandir}/man1/wwald.1
install -Dpm0644 docs/man/wwal-config.5 %{buildroot}%{_mandir}/man5/wwal-config.5

ln -s wwal %{buildroot}%{_bindir}/swww
ln -s wwald %{buildroot}%{_bindir}/swww-daemon
ln -s wwal %{buildroot}%{_bindir}/awww
ln -s wwald %{buildroot}%{_bindir}/awww-daemon

%check
%meson_test --suite waywal:phase1

%post
%systemd_user_post wwald.service wwald.socket

%preun
%systemd_user_preun wwald.service wwald.socket

%files
%license LICENSE
%doc README.md
%{_bindir}/wwal
%{_bindir}/wwald
%{_userunitdir}/wwald.service
%{_userunitdir}/wwald.socket
%{_datadir}/bash-completion/completions/wwal
%{_datadir}/zsh/site-functions/_wwal
%{_datadir}/fish/vendor_completions.d/wwal.fish
%{_mandir}/man1/wwal.1*
%{_mandir}/man1/wwald.1*
%{_mandir}/man5/wwal-config.5*

%files swww-compat
%{_bindir}/swww
%{_bindir}/swww-daemon
%{_bindir}/awww
%{_bindir}/awww-daemon

%changelog
* Sat Oct 24 2026 Execorn <execorn@users.noreply.github.com> - 1.0.0-1
- Initial release of wwal 1.0.0
