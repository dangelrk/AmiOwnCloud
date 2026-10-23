# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
# AmiOwnCloud -- Top-Level-Makefile. "make help" zeigt alle Ziele; Details in
# docs/BUILD.md. Die eigentliche Logik liegt in core/mk/project.mk
# (AmiCloudCore-Submodul), jedes Programm hat zusaetzlich sein eigenes
# Makefile.pc / Makefile.amiga-cross / CMakeLists.txt.

PROJECT    = AmiOwnCloud
PC_DIRS    = apps/amiowncloud-sync apps/amiowncloud-synctest
QT_DIRS    = 
AMIGA_DIRS = apps/amiowncloud-sync apps/amiowncloud-synctest apps/amiowncloud-sync-cx apps/amiowncloud-sync-prefs apps/amiowncloud-prefs handler/owncloudhandler

# Zusaetzlich in die Amiga-Installationsschublade (make dist-amiga):
# DOSDriver fuer den OWNCLOUD:-Handler -> <Schublade>/Storage/DOSDrivers/
DIST_EXTRA = handler/owncloudhandler/mount/OWNCLOUD:Storage/DOSDrivers

# Without the AmiCloudCore submodule there is no core/mk/project.mk --
# say so instead of make's bare "No such file or directory".
ifeq ($(wildcard core/mk/project.mk),)
$(error core/ fehlt -- das AmiCloudCore-Submodul holen: git submodule update --init)
endif

include core/mk/project.mk
