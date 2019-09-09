Name:           mesa
Version:        18.3.6
Release:        0
License:        MIT and Apache-2.0 and SGI Free Software License B v2.0 and BSD-3-Clause
Summary:        System for rendering interactive 3-D graphics
Url:            http://www.mesa3d.org
Group:          Graphics & UI Framework/Hardware Adaptation
Source:         %{name}-%{version}.tar.gz
Source1001:     %{name}.manifest
Source1002:     99-GPU-Acceleration.rules

BuildRequires:  meson
BuildRequires:  ninja
BuildRequires:  bison
BuildRequires:  fdupes
BuildRequires:  flex
BuildRequires:  gcc-c++
BuildRequires:  gettext-tools
BuildRequires:  libtool
BuildRequires:  libxml2-python
BuildRequires:  pkgconfig
BuildRequires:  python
BuildRequires:  python-lxml
BuildRequires:  python-xml
BuildRequires:  python3-mako
BuildRequires:  pkgconfig(expat)
BuildRequires:  pkgconfig(libdrm) >= 2.4.75
BuildRequires:  pkgconfig(libudev) > 150
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(wayland-server)
BuildRequires:  pkgconfig(wayland-protocols)
BuildRequires:  pkgconfig(tpl-egl)
BuildRequires:  pkgconfig(libtbm)
BuildRequires:  pkgconfig(libtdm)
BuildRequires:  pkgconfig(zlib)
BuildRequires:  pkgconfig(dlog)
%ifarch x86_64 %ix86
BuildRequires:  pkgconfig(libdrm_intel) >= 2.4.24
%endif

%if "%{_with_emulator}" == "1"
ExclusiveArch:
%endif

%description
Mesa is a 3-D graphics library with an API which is very similar to
that of OpenGL.* To the extent that Mesa utilizes the OpenGL command
syntax or state machine, it is being used with authorization from
Silicon Graphics, Inc.(SGI). However, the author does not possess an
OpenGL license from SGI, and makes no claim that Mesa is in any way a
compatible replacement for OpenGL or associated with SGI. Those who
want a licensed implementation of OpenGL should contact a licensed
vendor.

Please do not refer to the library as MesaGL (for legal reasons). It's
just Mesa or The Mesa 3-D graphics library.

* OpenGL is a trademark of Silicon Graphics Incorporated.

%prep
%setup -q -n %{name}-%{version}
cp %{SOURCE1001} .
cp %{SOURCE1002} .

%build
%{?asan:/usr/bin/gcc-unforce-options}
mkdir build/
meson build/ \
	--debug \
	--auto-features=disabled \
	--prefix=%{_prefix} \
	--libdir=%{_libdir} \
	-Dbuildtype=debug \
	-Dlibunwind=false \
	-Dglx=disabled \
	-Ddri3=false \
	-Dvulkan-drivers= \
	-Dosmesa=none \
	-Degl=true \
	-Dgles1=true \
	-Dgles2=true \
	-Dshared-glapi=true \
	-Dgbm=true \
	-Dplatforms=tizen \
	-Ddri-drivers=swrast \
	-Dgallium-drivers=etnaviv,kmsro \
	-Dtools=etnaviv

ninja -v %{?jobs:-j%jobs} -C build/

%install
DESTDIR=%{buildroot} ninja -v -C build/ install

mkdir -p %{buildroot}%{_libdir}/driver
cp -a  %{buildroot}%{_libdir}/libEGL* %{buildroot}%{_libdir}/driver
cp -a  %{buildroot}%{_libdir}/libGLES* %{buildroot}%{_libdir}/driver

mkdir -p %{buildroot}/etc/udev/rules.d
cp 99-GPU-Acceleration.rules %{buildroot}/etc/udev/rules.d

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%define _unpackaged_files_terminate_build 0
%manifest %{name}.manifest
%defattr(-,root,root)
%license COPYING
%{_libdir}/libglapi*
%{_libdir}/driver/*
%{_libdir}/dri/*
### FIXME OSO: this files supports only DRM, not TBM
###%{_libdir}/libgbm*
/etc/udev/rules.d/99-GPU-Acceleration.rules
