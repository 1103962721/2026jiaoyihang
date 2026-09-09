@echo off
call "E:\VisualStudio\VisualStudioIDE\VC\Auxiliary\Build\vcvars64.bat"
set "ROOT=%~dp0"
cd /d "%ROOT%DF_PlayClick_Client_Qt"
"%ROOT%.qt\6.6.3\msvc2019_64_static\bin\qmake.exe" -tp vc DF_PlayClick_Client_Qt.pro
cd /d "%ROOT%DF_PlayClick_Server_Qt"
"%ROOT%.qt\6.6.3\msvc2019_64_static\bin\qmake.exe" -tp vc DF_PlayClick_Server_Qt.pro
