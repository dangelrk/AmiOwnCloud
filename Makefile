# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
# AmiOwnCloud -- Top-Level-Makefile. "make help" zeigt alle Ziele; Details in
# docs/BUILD.md. Die eigentliche Logik liegt in core/mk/project.mk
# (AmiCloudCore-Submodul), jedes Programm hat zusaetzlich sein eigenes
# Makefile.pc / Makefile.amiga-cross / CMakeLists.txt.

PROJECT    = AmiOwnCloud
PC_DIRS    = apps/webdavsync apps/webdavsynctest
QT_DIRS    = 
AMIGA_DIRS = apps/webdavsync apps/webdavsynctest apps/amicloud-sync-cx apps/amicloudsync-prefs apps/owncloud-prefs handler/owncloudhandler

# Zusaetzlich in die Amiga-Installationsschublade (make dist-amiga):
# DOSDriver fuer den OWNCLOUD:-Handler -> <Schublade>/Storage/DOSDrivers/
DIST_EXTRA = handler/owncloudhandler/mount/OWNCLOUD:Storage/DOSDrivers

include core/mk/project.mk
