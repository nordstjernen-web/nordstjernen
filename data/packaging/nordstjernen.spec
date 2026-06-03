Name:           nordstjernen
Version:        0.8.1
Release:        1%{?dist}
Summary:        Clean-room, hardened web browser written from scratch in C

License:        LicenseRef-NSL-1.0
URL:            https://github.com/nordstjernen-web/nordstjernen
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  meson >= 1.0
%if 0%{?fedora} || 0%{?rhel}
BuildRequires:  ninja-build
%else
BuildRequires:  ninja
%endif
BuildRequires:  pkgconfig
BuildRequires:  cmake
BuildRequires:  pkgconfig(gtk4) >= 4.6
BuildRequires:  pkgconfig(libcurl) >= 7.85
BuildRequires:  pkgconfig(uchardet)
BuildRequires:  pkgconfig(libpsl)
BuildRequires:  pkgconfig(librsvg-2.0) >= 2.46
BuildRequires:  pkgconfig(libseccomp)
BuildRequires:  pkgconfig(fontconfig)
BuildRequires:  pkgconfig(pangoft2)

Recommends:     webp-pixbuf-loader
Recommends:     mpv

ExclusiveOS:    linux

%description
Nordstjernen is an independent, lightweight web browser built entirely
from scratch in C, using GTK 4 for the UI and libcurl for networking.
It is a clean-room implementation with no upstream browser engine: the
HTML parser (lexbor), the JavaScript interpreter (QuickJS), and the
image decoder (Wuffs) are all integrated in-tree. The engine is a
hardened, zero-JIT HTML/CSS renderer aimed at secure general web
browsing, document reading, embedded systems, and embedding in other
applications. It does not phone home and does not telemeter the user.

%prep
%autosetup -n %{name}-%{version}

%build
%meson
%meson_build

%install
%meson_install

%files
%license License.md
%doc README.md
%{_bindir}/nordstjernen

%changelog
* Sun May 31 2026 Andreas Røsdal <andreas.rosdal@gmail.com> - 0.8.1-1
- Release 0.8.1.
