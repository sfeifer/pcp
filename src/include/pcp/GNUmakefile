#
# Copyright (c) 2013-2019,2021 Red Hat.
# Copyright (c) 2000,2004 Silicon Graphics, Inc.  All Rights Reserved.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2 of the License, or (at your
# option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.
#

TOPDIR = ../../..
include $(TOPDIR)/src/include/builddefs
-include ./GNUlocaldefs

INIH_HFILES = ini.h
SDSH_HFILES = sds.h sdsalloc.h
EXTERNAL_HFILES = dict.h
HFILES = pmapi.h impl.h pmda.h pmtime.h pmdaroot.h pmafm.h \
	 trace.h trace_dev.h mmv_stats.h mmv_dev.h import.h \
	 pmjson.h pmhttp.h pmdbg.h pmwebapi.h deprecated.h \
	 ini.h sds.h dict.h archive.h
INFILES = config.h.in platform_defs.h.in
CONFFILES = config.h platform_defs.h
ifeq "$(ENABLE_MULTILIB)" "true"
HFILES += config32.h config64.h platform32.h platform64.h
else
INFILES += configsz.h.in platformsz.h.in
CONFFILES += configsz.h platformsz.h
endif
GENERATED_HFILES = $(CONFFILES)
NOSHIP_HFILES = libpcp.h sdsalloc.h

LDIRT = $(GENERATED_HFILES) $(INIH_HFILES) $(SDSH_HFILES) $(EXTERNAL_HFILES)

default :: default_pcp

default_pcp : $(HEADERS) $(GENERATED_HFILES) $(INIH_HFILES) $(SDSH_HFILES) $(EXTERNAL_HFILES)

include $(BUILDRULES)

$(EXTERNAL_HFILES):
	$(LN_S) -f $(TOPDIR)/src/external/$@ .
$(INIH_HFILES):
	$(LN_S) -f $(TOPDIR)/vendor/github.com/benhoyt/inih/$@ .
$(SDSH_HFILES):
	$(LN_S) -f $(TOPDIR)/vendor/github.com/redis/hiredis/$@ .

install :: default_pcp install_pcp

install_pcp : default_pcp
	$(INSTALL) -m 644 $(HFILES) $(GENERATED_HFILES) $(PCP_INC_DIR)
