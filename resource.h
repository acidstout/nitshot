/* Shared by the .c files and nitshot.rc */
#ifndef NITSHOT_RESOURCE_H
#define NITSHOT_RESOURCE_H

#define IDI_APPICON     101   /* the app's face, and the tray icon */

#define IDD_SETTINGS    200

#define IDC_COPYCLIP    1001
#define IDC_SAVEDISK    1002
#define IDC_FOLDER      1003
#define IDC_BROWSE      1004

#define IDC_HOOKWSS     1010
#define IDC_HOOKPRTSC   1011
#define IDC_PRTSCFULL   1012

#define IDC_DIM         1020
#define IDC_TOAST       1022

#define IDC_HDRSTATUS   1030
#define IDC_JXR         1031
#define IDC_SIDECAR     1032
#define IDC_ROLLOFF     1033
#define IDC_QUALITY     1034

#define IDC_AUTOSTART   1040
#define IDC_DEBUGLOG    1041
#define IDC_FORCEGDI    1042
#define IDC_FORCESDR    1043


#define IDC_RECAUDIO    1060
#define IDC_RECFPS      1061
#define IDC_RECENC      1062
#define IDC_RECHDR      1063
#define IDC_RECWAV      1064
#define IDC_RECMUX      1065

/* Labels that used to be -1. Translating them at runtime means addressing
   them, and a control with no id cannot be addressed. */
#define IDC_LBL_FOLDER   1070
#define IDC_LBL_DIM      1071
#define IDC_LBL_PERCENT  1072
#define IDC_LBL_QUALITY  1073
#define IDC_LBL_SOUND    1074
#define IDC_LBL_FPS      1075
#define IDC_LBL_VIDEO    1076
#define IDC_LBL_LANGUAGE 1077
#define IDC_LANGUAGE     1078

/* The tabbed layout. */
#define IDC_TABS         1080
#define IDC_LBL_VIDFOLDER 1081
#define IDC_VIDFOLDER    1082
#define IDC_VIDBROWSE    1083
#define IDC_APPLY        1084

#define APP_VER_MAJOR   0
#define APP_VER_MINOR   8
#define APP_VER_PATCH   0
#define APP_VER_BUILD   0
#define APP_VER_STRING  "0.8.0.0"
#define APP_VER_WSTRING L"0.8.0.0"

#endif /* NITSHOT_RESOURCE_H */
