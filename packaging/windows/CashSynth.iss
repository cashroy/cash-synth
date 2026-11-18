; Inno Setup 6 script: the Windows installer for Cash Synth (64-bit).
; Installs the VST3 into the common VST3 folder and the Standalone app into
; Program Files, each as a component that can be left out.
;
;   iscc /DAppVersion=1.0.0 /DArtefacts=<build>\CashSynth_artefacts\Release /DOutDir=dist packaging\windows\CashSynth.iss

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef Artefacts
  #define Artefacts "..\..\build\CashSynth_artefacts\Release"
#endif
#ifndef OutDir
  #define OutDir "..\..\dist"
#endif

[Setup]
AppId={{5E0C91DA-35D3-4188-B257-ABA97688E92D}
AppName=Cash Synth
AppVersion={#AppVersion}
AppVerName=Cash Synth {#AppVersion}
AppPublisher=Cashroy
AppPublisherURL=https://cashroy.github.io/cashsynth/
AppSupportURL=https://github.com/cashroy/cash-synth/issues
DefaultDirName={autopf}\Cashroy\Cash Synth
DefaultGroupName=Cashroy
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE
OutputDir={#OutDir}
OutputBaseFilename=CashSynth-Setup
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
UninstallDisplayName=Cash Synth {#AppVersion}

[Types]
Name: "full"; Description: "VST3 plug-in and Standalone app"
Name: "custom"; Description: "Pick what to install"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plug-in (Common Files\VST3)"; Types: full custom
Name: "app"; Description: "Standalone app"; Types: full custom

[Files]
Source: "{#Artefacts}\VST3\Cash Synth.vst3\*"; DestDir: "{commoncf64}\VST3\Cash Synth.vst3"; Components: vst3; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "{#Artefacts}\Standalone\Cash Synth.exe"; DestDir: "{app}"; Components: app; Flags: ignoreversion
Source: "..\..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{group}\Cash Synth"; Filename: "{app}\Cash Synth.exe"; Components: app
Name: "{group}\Uninstall Cash Synth"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\Cash Synth.exe"; Description: "Open Cash Synth now"; Flags: nowait postinstall skipifsilent; Components: app
