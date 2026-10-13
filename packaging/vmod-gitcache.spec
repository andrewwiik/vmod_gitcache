%global debug_package %{nil}
%global vp_version 6.0.18r5

Name:           vmod-gitcache
Version:        0.1.0
Release:        1%{?dist}
Summary:        VMOD for caching git smart-HTTP, dumb HTTP and Git LFS on Varnish Enterprise
License:        BSD-2-Clause
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc, make, python3, pkgconfig
BuildRequires:  openssl-devel
BuildRequires:  varnish-plus-devel = %{vp_version}
# VMOD ABI is strict: the module only loads into the exact build it was compiled for.
Requires:       varnish-plus = %{vp_version}

%description
Classifies git smart-HTTP requests (v0, v1, v2), dumb HTTP and Git LFS traffic
and builds stable, credential-scoped cache keys, including canonical hashing of
upload-pack request bodies. See PROTOCOL.md for the behaviour and its tests.

%prep
%setup -q

%build
make all B=build INC=/usr/include/varnish-plus VMODTOOL=/usr/share/varnish-plus/vmodtool.py CFLAGS="%{optflags} -Wno-unused-parameter"

%install
install -D -m 0755 build/libvmod_gitcache.so %{buildroot}%{_libdir}/varnish-plus/vmods/libvmod_gitcache.so
install -D -m 0644 PROTOCOL.md %{buildroot}%{_docdir}/%{name}/PROTOCOL.md
install -D -m 0644 tests/e2e/git.vcl.in %{buildroot}%{_docdir}/%{name}/examples/git.vcl.in
install -D -m 0644 tests/e2e/upstream.vcl.in %{buildroot}%{_docdir}/%{name}/examples/upstream.vcl.in

%files
%{_libdir}/varnish-plus/vmods/libvmod_gitcache.so
%doc %{_docdir}/%{name}

%changelog
* Tue Oct 13 2026 vmod-gitcache <noreply@example.invalid> - 0.1.0-1
- Initial package
