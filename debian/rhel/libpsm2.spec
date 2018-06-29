#
#  This file is provided under a dual BSD/GPLv2 license.  When using or
#  redistributing this file, you may do so under either license.
#
#  GPL LICENSE SUMMARY
#
#  Copyright(c) 2017 Intel Corporation.
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of version 2 of the GNU General Public License as
#  published by the Free Software Foundation.
#
#  This program is distributed in the hope that it will be useful, but
#  WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
#  General Public License for more details.
#
#  Contact Information:
#  Intel Corporation, www.intel.com
#
#  BSD LICENSE
#
#  Copyright(c) 2017 Intel Corporation.
#
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions
#  are met:
#
#    * Redistributions of source code must retain the above copyright
#      notice, this list of conditions and the following disclaimer.
#    * Redistributions in binary form must reproduce the above copyright
#      notice, this list of conditions and the following disclaimer in
#      the documentation and/or other materials provided with the
#      distribution.
#    * Neither the name of Intel Corporation nor the names of its
#      contributors may be used to endorse or promote products derived
#      from this software without specific prior written permission.
#
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
#  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
#  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
#  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
#  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
#  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
#  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
#  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
#  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
Summary: Intel PSM2 Libraries
Name: libpsm2
Version: 10.3.35
Release: 1
License: BSD or GPLv2
URL: https://github.com/01org/opa-psm2/

# The tarball can be created by:
# git clone https://github.com/01org/opa-psm2
# cd opa-psm2
# git checkout 81a3f06cb764af3238ac326c5f69e24393bae85f
# make dist
Source0: libpsm2-%{version}.tar.gz

# The OPA product is supported on x86_64 only:
ExclusiveArch: x86_64

BuildRequires: gcc
Provides: hfi1-psm
Obsoletes: hfi1-psm < 1.0.0

%if ""
%package -n libpsm2
%endif
Summary: Intel PSM2 Libraries
Provides: libpsm2 = %{version}-%{release}
Provides: libpsm2%{_isa} = %{version}-%{release}
%if 0%{?suse_version}
BuildRequires: libnuma-devel
Requires: libnuma1
%else
%if 0%{?rhel}==0 || 0%{?rhel} > 6
BuildRequires: systemd
BuildRequires: numactl-devel
Requires: numactl-libs
%endif
%endif

%package -n libpsm2-devel
Summary: Development files for Intel PSM2
Requires: %{name}%{?_isa} = %{version}-%{release}
Provides: hfi1-psm-devel
Obsoletes: hfi1-psm-devel < 1.0.0

%package -n libpsm2-compat
Summary: Compat library for Intel PSM2
Requires: %{name}%{?_isa} = %{version}-%{release}
%if 0%{?fedora}
Requires: systemd-udev
%endif
Provides: hfi1-psm-compat
Obsoletes: hfi1-psm-compat < 1.0.0

# If an alternate basename is defined, like in SLES >=12.3
# Then we generate a different base src.rpm, so use this
# description instead.
%if ""
%description
The source code for the PSM2 messaging API, libpsm2.
A low-level user-level communications interface for the Intel(R) OPA
family of products. PSM2 users are enabled with mechanisms
necessary to implement higher level communications
interfaces in parallel environments.
%endif

# In distro's other than SLES >=12.3 we use a single description
# for both the .src.rpm and the base binary rpm. As the
# RPM_NAME_BASEEXT defaults to empty contents.
%description -n libpsm2
PSM2 Messaging API, or PSM2 API, is the low-level
user-level communications interface for the Intel(R) OPA
family of products. PSM2 users are enabled with mechanisms
necessary to implement higher level communications
interfaces in parallel environments.

%description -n libpsm2-devel
Intel(R) PSM2, psm2*.h, headers and libpsm2.so files necessary
for developing software using libpsm2.

%description -n libpsm2-compat
Support for MPIs linked with PSM versions < 2. This will allow
software compiled to use Intel(R) Truescale PSM, libinfinipath, to run
with Intel(R) OPA PSM2, libpsm2.

%prep
%setup -q -n libpsm2-%{version}

%build
export CFLAGS="%{optflags}"
make %{?_smp_mflags}

%install
%make_install

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files -n libpsm2
%if 0%{?rhel} && 0%{?rhel} < 7
%{!?_licensedir:%global license %doc}
%endif
%license COPYING
%{_libdir}/libpsm2.so.2.1
%{_libdir}/libpsm2.so.2
%{_udevrulesdir}/40-psm.rules

%files -n libpsm2-devel
%{_libdir}/libpsm2.so
%{_libdir}/libpsm2.a
%{_includedir}/psm2.h
%{_includedir}/psm2_mq.h
%{_includedir}/psm2_am.h
%{_includedir}/hfi1diag

%files -n libpsm2-compat
%{_libdir}/psm2-compat
%if 0%{?rhel} && 0%{?rhel} < 7
/usr/lib/udev/rules.d/40-psm-compat.rules
%else
%{_udevrulesdir}/40-psm-compat.rules
%endif
/etc/modprobe.d/libpsm2-compat.conf
%{_prefix}/lib/libpsm2

%changelog
* Wed Aug 30 2017 Rusell McGuire <russell.w.mcguire@intel.com>
- Adjust RPM names to match SLES 12.3 distro names
* Tue Apr 05 2016 Paul Reger <paul.j.reger@intel.com>
- Upstream PSM2 source code for Fedora.
