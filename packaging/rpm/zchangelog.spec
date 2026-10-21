Name:           zchangelog
Version:        0.2.0
Release:        1%{?dist}
Summary:        Change stream for ZFS datasets
License:        CDDL-1.0
URL:            https://github.com/lhjnano/zchangelog
Source0:        %{name}-%{version}.tar.gz
BuildRequires:  gcc make autoconf automake pkgconfig sqlite-devel

%description
Watches a dataset mount with fanotify, appends change records to a
SQLite journal with sequence-number cursors, and serves multiple
named consumers with register/read/clear semantics.

%prep
%setup -q

%build
%configure
%make_build

%install
%make_install

%files
%{_bindir}/zchangelog
%license COPYING

%changelog
* Wed Oct 21 2026 HeonJe Lee <lhjnano@gmail.com> - 0.2.0-1
- Add the inotify collection backend (watch -b inotify) for kernels
  without fanotify FID reporting.
- Add the maint functional test.

* Sat Oct 17 2026 HeonJe Lee <lhjnano@gmail.com> - 0.1.0-1
- Initial package.
