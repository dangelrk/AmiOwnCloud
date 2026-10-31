# Workbench-Icons der Installationsschublade (make dist-amiga), Format
# siehe core/tools/mkicon.py. Positionen: Mitte des Icons minus 18.
.                          drawer  drawer    win=60,30,540,220
Install-AmiOwnCloud        project install   tool=Installer tt=APPNAME=AmiOwnCloud tt=MINUSER=AVERAGE tt=DEFUSER=AVERAGE pos=72,6
Uninstall-AmiOwnCloud      project uninstall tool=C:IconX tt=WINDOW=CON:0/20/640/200/Uninstall-AmiOwnCloud/CLOSE/WAIT pos=262,6
ReadMe                     project doc       tool=SYS:Utilities/MultiView pos=422,6
Docs                       drawer  drawer    win=80,50,300,100 pos=42,70
C                          drawer  drawer    win=80,50,460,100 pos=132,70
Examples                   drawer  drawer    win=80,50,520,160 pos=222,70
Storage                    drawer  drawer    win=80,50,300,100 pos=322,70
LICENSE                    project doc       tool=SYS:Utilities/MultiView pos=42,132
THIRD-PARTY-NOTICES        project doc       tool=SYS:Utilities/MultiView pos=192,132
Install-AmiOwnCloud.shell  project doc       tool=C:IconX tt=WINDOW=CON:0/20/640/300/Install-AmiOwnCloud/CLOSE/WAIT pos=372,132

Docs/AmiOwnCloud.guide     project guide     tool=SYS:Utilities/MultiView pos=82,6
C/AmiOwnCloudPrefs         tool    prefs     stack=32768 pos=52,6
C/AmiOwnCloudSync          tool    sync      stack=65536 tt=DONOTWAIT tt=CX_POPKEY=ctrl%20alt%20a pos=202,6
C/AmiOwnCloudSyncPrefs     tool    prefs     stack=32768 pos=362,6
Examples/amiowncloud.cfg.example      project doc tool=SYS:Utilities/MultiView pos=112,6
Examples/AmiOwnCloudSync.cfg.example  project doc tool=SYS:Utilities/MultiView pos=362,6
Examples/amiowncloud-sync.cfg.example project doc tool=SYS:Utilities/MultiView pos=112,70
Examples/OWNCLOUD.mountlist           project doc tool=SYS:Utilities/MultiView pos=362,70
Storage/DOSDrivers         drawer  drawer    win=80,50,300,100 pos=82,6
Storage/DOSDrivers/OWNCLOUD project mount    tool=C:Mount pos=82,6
