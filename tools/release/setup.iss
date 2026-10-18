; SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
; SPDX-License-Identifier: GPL-2.0-or-later
;
; The release's setup .exe (Inno Setup 6; make-release.ps1 compiles it with ISCC): installs the
; staged release folder for the current user (no administrator), offers a desktop shortcut to
; BLauncher.exe, and on uninstall lets the launcher remove the game view it made (links to the
; player's game, removed as links) before the files go (Kaniel 2026-10-13: one setup file, no bat
; files). The saves, settings and logs in user\ stay after an uninstall.
;
;   ISCC /DAppVersion=0.1.3 /DSourceDir=<staged release> /DOutputDir=<dir> /DIconFile=<ico> [/DStyleFile=<vsf>] setup.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#define AppName "Bloodborne Seamless Co-op"

[Setup]
AppId={{6E0B4C2A-3F7D-4B8E-9A51-2C8D7E4F1B63}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppName}
AppPublisherURL=https://github.com/lKaniel/bloodborne-server
AppSupportURL=https://github.com/lKaniel/bloodborne-server/issues
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=BloodborneSeamlessCoop-v{#AppVersion}-setup
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\BLauncher.exe
UninstallDisplayName={#AppName}
Compression=lzma2
SolidCompression=yes
; The launcher's look (Kaniel 2026-10-17): Inno's dark style recolored to the launcher's slate,
; parchment and gold (setup_style.py; make-release.ps1 passes it as StyleFile) with its own title bar
; and frame, the hunter logo and night street (setup_art.py draws tools\release\art), gold titles from
; [Code], and no box, disc or folder pictures.
WizardStyle=modern dark hidebevels includetitlebar
#ifdef StyleFile
WizardStyleFile={#StyleFile}
#endif
WizardBackColor=#0d0f13
WizardImageFile=art\wizard-202.png,art\wizard-269.png,art\wizard-336.png,art\wizard-430.png,art\wizard-534.png
WizardImageBackColor=#12151a
WizardSmallImageFile=art\small-58.png,art\small-77.png,art\small-97.png,art\small-124.png,art\small-159.png
WizardSmallImageBackColor=#0d0f13
DisableWelcomePage=no
; The language is Windows' own (any of [Languages]); the pick list (its focused row stays Windows
; blue) only when no translation matches it.
ShowLanguageDialog=auto
; The game, its shader cache and the saves need room beside the install.
ExtraDiskSpaceRequired=1073741824
CloseApplications=yes

[Languages]
; Every translation Inno Setup has: its own in the compiler, the rest (official ones this Inno lacks
; and the community ones) in tools\release\languages from jrsoftware/issrc Files\Languages.
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "arabic"; MessagesFile: "compiler:Languages\Arabic.isl"
Name: "armenian"; MessagesFile: "compiler:Languages\Armenian.isl"
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "bulgarian"; MessagesFile: "compiler:Languages\Bulgarian.isl"
Name: "catalan"; MessagesFile: "compiler:Languages\Catalan.isl"
Name: "corsican"; MessagesFile: "compiler:Languages\Corsican.isl"
Name: "czech"; MessagesFile: "compiler:Languages\Czech.isl"
Name: "danish"; MessagesFile: "compiler:Languages\Danish.isl"
Name: "dutch"; MessagesFile: "compiler:Languages\Dutch.isl"
Name: "finnish"; MessagesFile: "compiler:Languages\Finnish.isl"
Name: "french"; MessagesFile: "compiler:Languages\French.isl"
Name: "german"; MessagesFile: "compiler:Languages\German.isl"
Name: "hebrew"; MessagesFile: "compiler:Languages\Hebrew.isl"
Name: "hungarian"; MessagesFile: "compiler:Languages\Hungarian.isl"
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"
Name: "norwegian"; MessagesFile: "compiler:Languages\Norwegian.isl"
Name: "polish"; MessagesFile: "compiler:Languages\Polish.isl"
Name: "portuguese"; MessagesFile: "compiler:Languages\Portuguese.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "slovak"; MessagesFile: "compiler:Languages\Slovak.isl"
Name: "slovenian"; MessagesFile: "compiler:Languages\Slovenian.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "swedish"; MessagesFile: "compiler:Languages\Swedish.isl"
Name: "tamil"; MessagesFile: "compiler:Languages\Tamil.isl"
Name: "thai"; MessagesFile: "compiler:Languages\Thai.isl"
Name: "turkish"; MessagesFile: "compiler:Languages\Turkish.isl"
Name: "ukrainian"; MessagesFile: "compiler:Languages\Ukrainian.isl"
Name: "abkhazian"; MessagesFile: "languages\Abkhazian.isl"
Name: "afrikaans"; MessagesFile: "languages\Afrikaans.isl"
Name: "albanian"; MessagesFile: "languages\Albanian.isl"
Name: "azerbaijan"; MessagesFile: "languages\Azerbaijan.isl"
Name: "basque"; MessagesFile: "languages\Basque.isl"
Name: "belarusian"; MessagesFile: "languages\Belarusian.isl"
Name: "bengali"; MessagesFile: "languages\Bengali.islu"
Name: "bosnian"; MessagesFile: "languages\Bosnian.isl"
Name: "chinesesimplified"; MessagesFile: "languages\ChineseSimplified.isl"
Name: "chinesetraditional"; MessagesFile: "languages\ChineseTraditional.isl"
Name: "croatian"; MessagesFile: "languages\Croatian.isl"
Name: "englishbritish"; MessagesFile: "languages\EnglishBritish.isl"
Name: "esperanto"; MessagesFile: "languages\Esperanto.isl"
Name: "estonian"; MessagesFile: "languages\Estonian.isl"
Name: "ewe"; MessagesFile: "languages\Ewe.isl"
Name: "farsi"; MessagesFile: "languages\Farsi.isl"
Name: "galician"; MessagesFile: "languages\Galician.isl"
Name: "georgian"; MessagesFile: "languages\Georgian.isl"
Name: "greek"; MessagesFile: "languages\Greek.isl"
Name: "hindi"; MessagesFile: "languages\Hindi.islu"
Name: "icelandic"; MessagesFile: "languages\Icelandic.isl"
Name: "indonesian"; MessagesFile: "languages\Indonesian.isl"
Name: "kazakh"; MessagesFile: "languages\Kazakh.islu"
Name: "kurdish"; MessagesFile: "languages\Kurdish.isl"
Name: "latvian"; MessagesFile: "languages\Latvian.isl"
Name: "ligurian"; MessagesFile: "languages\Ligurian.isl"
Name: "lithuanian"; MessagesFile: "languages\Lithuanian.isl"
Name: "luxemburgish"; MessagesFile: "languages\Luxemburgish.isl"
Name: "macedonian"; MessagesFile: "languages\Macedonian.isl"
Name: "malaysian"; MessagesFile: "languages\Malaysian.isl"
Name: "mongolian"; MessagesFile: "languages\Mongolian.isl"
Name: "nepali"; MessagesFile: "languages\Nepali.islu"
Name: "occitan"; MessagesFile: "languages\Occitan.isl"
Name: "romanian"; MessagesFile: "languages\Romanian.isl"
Name: "scottishgaelic"; MessagesFile: "languages\ScottishGaelic.isl"
Name: "serbiancyrillic"; MessagesFile: "languages\SerbianCyrillic.isl"
Name: "serbianlatin"; MessagesFile: "languages\SerbianLatin.isl"
Name: "sinhala"; MessagesFile: "languages\Sinhala.islu"
Name: "tatar"; MessagesFile: "languages\Tatar.isl"
Name: "urdu"; MessagesFile: "languages\Urdu.isl"
Name: "uyghur"; MessagesFile: "languages\Uyghur.islu"
Name: "uzbek"; MessagesFile: "languages\Uzbek.isl"
Name: "valencian"; MessagesFile: "languages\Valencian.isl"
Name: "vietnamese"; MessagesFile: "languages\Vietnamese.isl"
Name: "asturian"; MessagesFile: "languages\Asturian.isl"
Name: "marathi"; MessagesFile: "languages\Marathi.islu"
Name: "montenegrin"; MessagesFile: "languages\Montenegrin.isl"
Name: "norwegiannynorsk"; MessagesFile: "languages\NorwegianNynorsk.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; The player's own files are never installed over (check_release.py keeps them out of a release as well):
; an update leaves the settings, key and pad remaps (bbport.ini), mods and patch choices as they are.
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "bbport.ini,mods.json,patches.json"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\BLauncher.exe"; WorkingDir: "{app}"
Name: "{autoprograms}\{#AppName} - read me"; Filename: "{app}\READ ME FIRST.txt"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\BLauncher.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\BLauncher.exe"; WorkingDir: "{app}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
; The launcher's update runs this setup silently (bbport_coop.setup_command): the launcher starts again.
Filename: "{app}\BLauncher.exe"; WorkingDir: "{app}"; Flags: nowait skipifnotsilent

[UninstallRun]
Filename: "{app}\BLauncher.exe"; Parameters: "--coop-uninstall"; WorkingDir: "{app}"; Flags: runhidden waituntilterminated; RunOnceId: "CoopUninstall"

[Code]
// The launcher's GOLD and MUTED (bbport_launcher_win.py) as $bbggrr; the rest comes from the style.
const
  Gold = $78b8d9;
  Muted = $90a5ae;

// A label takes our color only once the style stops drawing its font.
procedure Ink(L: TNewStaticText; Color: Integer);
begin
  L.StyleElements := L.StyleElements - [seFont];
  L.Font.Color := Color;
end;

// Palatino has Latin, Greek and Cyrillic only: other scripts keep Windows' own font for titles.
function SerifScript: Boolean;
begin
  Result := Pos(',' + ActiveLanguage + ',', ',arabic,armenian,bengali,chinesesimplified,chinesetraditional,' +
    'farsi,georgian,hebrew,hindi,japanese,korean,marathi,nepali,sinhala,tamil,thai,urdu,uyghur,') = 0;
end;

// Titles as the launcher's: gold serif, not bold.
procedure Title(L: TNewStaticText; Size: Integer);
begin
  Ink(L, Gold);
  if SerifScript then
    L.Font.Name := 'Palatino Linotype';
  L.Font.Size := Size;
  L.Font.Style := [];
end;

// The page title in its serif with the page's hint under it.
procedure Header(Name, Description: TNewStaticText; Grow: Boolean);
begin
  Title(Name, 12);
  Name.Top := ScaleY(8);
  Name.Height := ScaleY(26);
  if Grow then
    WizardForm.AdjustLabelHeight(Name);
  Description.Top := Name.Top + Name.Height + ScaleY(2);
  Ink(Description, Muted);
end;

// No folder or warning pictures: the text moves into their place (on its right in right-to-left
// languages, where the picture sits right of the text).
procedure DropPicture(Image: TBitmapImage; L: TNewStaticText);
begin
  Image.Visible := False;
  if Image.Left > L.Left then
    L.Width := Image.Left + Image.Width - L.Left
  else begin
    L.Width := L.Width + (L.Left - Image.Left);
    L.Left := Image.Left;
  end;
end;

// The big title of the first and last page, as tall as its lines (three in Russian), text under it.
procedure BigTitle(L, Below: TNewStaticText);
var
  Grown: Integer;
begin
  Title(L, 17);
  Grown := WizardForm.AdjustLabelHeight(L);
  Below.Top := Below.Top + Grown;
end;

// --- The launcher's own title bar (Kaniel 2026-10-17: custom, as in the launcher) ---------------
// Windows' bar goes (no border); a strip on the launcher's ground takes its place with the
// launcher's CaptionButton: thin glyphs, MUTED, PARCHMENT over a light wash under the pointer, the
// close button blood red. A 30 ms timer follows the pointer for the hover and hands a press on the
// empty strip to Windows' own window move.
const
  Ground = $130f0d;
  Parchment = $c2e2f0;
  HotWash = $282828;
  CloseHot = $262a8a;
  WM_NCLBUTTONDOWN = $00A1;
  WM_SYSCOMMAND = $0112;
  HTCAPTION = 2;
  SC_MINIMIZE = $F020;

var
  BarForm: TSetupForm;
  Bar: TPanel;
  BarMin, BarClose: TBitmapImage;
  BarTimer: LongWord;
  BarHot: Integer;
  BarHeld, BarMoving: Boolean;

function SetTimer(Wnd: HWND; IDEvent, Elapse: LongWord; TimerFunc: LongWord): LongWord;
  external 'SetTimer@user32.dll stdcall';
function KillTimer(Wnd: HWND; IDEvent: LongWord): BOOL;
  external 'KillTimer@user32.dll stdcall';
function GetCursorPos(var Point: TPoint): BOOL;
  external 'GetCursorPos@user32.dll stdcall';
function WindowFromPoint(X, Y: Longint): HWND;
  external 'WindowFromPoint@user32.dll stdcall';
function GetAsyncKeyState(Key: Integer): Integer;
  external 'GetAsyncKeyState@user32.dll stdcall';
function GetForegroundWindow: HWND;
  external 'GetForegroundWindow@user32.dll stdcall';
function ReleaseCapture: BOOL;
  external 'ReleaseCapture@user32.dll stdcall';
function DwmSetWindowAttribute(Wnd: HWND; Attribute: LongWord; var Value: LongWord; Size: LongWord): Longint;
  external 'DwmSetWindowAttribute@dwmapi.dll stdcall delayload';

// One window button: its glyph (minimize or close) drawn as the launcher draws it.
procedure PaintBarButton(Button: TBitmapImage; Close, Hot: Boolean);
var
  W, H, X, Y, S: Integer;
  Back: Integer;
begin
  W := Button.Width;
  H := Button.Height;
  X := W div 2;
  Y := H div 2;
  S := ScaleX(5);
  Back := Ground;
  if Hot and Close then
    Back := CloseHot
  else if Hot then
    Back := HotWash;
  Button.Bitmap.Width := W;
  Button.Bitmap.Height := H;
  with Button.Bitmap.Canvas do begin
    Brush.Color := Back;
    Pen.Color := Back;
    Rectangle(0, 0, W, H);
    if Hot then
      Pen.Color := Parchment
    else
      Pen.Color := Muted;
    Pen.Width := ScaleX(1);
    if Close then begin
      MoveTo(X - S, Y - S);
      LineTo(X + S + 1, Y + S + 1);
      MoveTo(X - S, Y + S);
      LineTo(X + S + 1, Y - S - 1);
    end else begin
      MoveTo(X - S, Y);
      LineTo(X + S + 1, Y);
    end;
  end;
  Button.Invalidate;
end;

procedure BarMinClick(Sender: TObject);
begin
  SendMessage(BarForm.Handle, WM_SYSCOMMAND, SC_MINIMIZE, 0);
end;

procedure BarCloseClick(Sender: TObject);
begin
  BarForm.Close;
end;

// Hover and moving: which button the pointer is over, and a press on the empty strip.
procedure BarTick(Wnd: HWND; Msg, IDEvent, Time: LongWord);
var
  P: TPoint;
  Over: Boolean;
  Hot, X: Integer;
  Down: Boolean;
begin
  if BarMoving or (Bar = nil) then
    exit;
  GetCursorPos(P);
  Over := (GetForegroundWindow = BarForm.Handle) and (WindowFromPoint(P.X, P.Y) = Bar.Handle);
  Hot := 0;
  X := P.X - BarForm.Left;
  if Over and (X >= BarMin.Left) and (X < BarMin.Left + BarMin.Width) then
    Hot := 1
  else if Over and (X >= BarClose.Left) then
    Hot := 2;
  if Hot <> BarHot then begin
    BarHot := Hot;
    PaintBarButton(BarMin, False, Hot = 1);
    PaintBarButton(BarClose, True, Hot = 2);
  end;
  Down := (GetAsyncKeyState($01) and $8000) <> 0;
  if Down and not BarHeld and Over and (Hot = 0) then begin
    BarMoving := True;
    ReleaseCapture;
    SendMessage(BarForm.Handle, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    BarMoving := False;
    Down := False;
  end;
  BarHeld := Down;
end;

function BarButton(Close: Boolean; Left: Integer): TBitmapImage;
begin
  Result := TBitmapImage.Create(Bar);
  Result.Parent := Bar;
  Result.SetBounds(Left, 0, ScaleX(46), Bar.Height);
  Result.Bitmap := TBitmap.Create;
  PaintBarButton(Result, Close, False);
end;

// Swaps Windows' bar for ours: the window keeps its page size and grows by the strip.
procedure LauncherBar(Form: TSetupForm);
var
  I, W, H, Tall: Integer;
  Bounds: array of TRect;
  Corner, Edge: LongWord;
begin
  BarForm := Form;
  Tall := ScaleY(34);
  W := Form.ClientWidth;
  H := Form.ClientHeight;
  // where everything sits now: the anchors would move it as the frame goes, so it is put back
  SetArrayLength(Bounds, Form.ControlCount);
  for I := 0 to Form.ControlCount - 1 do begin
    Bounds[I].Left := Form.Controls[I].Left;
    Bounds[I].Top := Form.Controls[I].Top;
    Bounds[I].Right := Form.Controls[I].Width;
    Bounds[I].Bottom := Form.Controls[I].Height;
  end;
  Form.BorderStyle := bsNone;
  Form.ClientWidth := W;
  Form.ClientHeight := H + Tall;
  for I := 0 to GetArrayLength(Bounds) - 1 do
    Form.Controls[I].SetBounds(Bounds[I].Left, Bounds[I].Top + Tall, Bounds[I].Right, Bounds[I].Bottom);
  Bar := TPanel.Create(Form);
  Bar.Parent := Form;
  Bar.SetBounds(0, 0, W, Tall);
  Bar.Anchors := [akLeft, akTop, akRight];
  Bar.BevelOuter := bvNone;
  Bar.StyleElements := [];
  Bar.ParentBackground := False;
  Bar.Color := Ground;
  BarClose := BarButton(True, W - ScaleX(46));
  BarClose.OnClick := @BarCloseClick;
  BarMin := BarButton(False, W - 2 * ScaleX(46));
  BarMin.OnClick := @BarMinClick;
  // Windows 11: round corners and a thin LINE edge, as other windows have
  Corner := 2;
  Edge := $38302b;
  DwmSetWindowAttribute(Form.Handle, 33, Corner, 4);
  DwmSetWindowAttribute(Form.Handle, 34, Edge, 4);
  BarTimer := SetTimer(0, 0, 30, CreateCallback(@BarTick));
end;

procedure StopBar;
begin
  if BarTimer <> 0 then
    KillTimer(0, BarTimer);
  BarTimer := 0;
  Bar := nil;
end;

procedure DeinitializeSetup;
begin
  StopBar;
end;

procedure DeinitializeUninstall;
begin
  StopBar;
end;

procedure InitializeWizard;
begin
  Header(WizardForm.PageNameLabel, WizardForm.PageDescriptionLabel, True);
  BigTitle(WizardForm.WelcomeLabel1, WizardForm.WelcomeLabel2);
  BigTitle(WizardForm.FinishedHeadingLabel, WizardForm.FinishedLabel);
  Ink(WizardForm.DiskSpaceLabel, Muted);
  Ink(WizardForm.FilenameLabel, Muted);
  DropPicture(WizardForm.SelectDirBitmapImage, WizardForm.SelectDirLabel);
  DropPicture(WizardForm.SelectGroupBitmapImage, WizardForm.SelectStartMenuFolderLabel);
  DropPicture(WizardForm.PreparingErrorBitmapImage, WizardForm.PreparingLabel);
  LauncherBar(WizardForm);
end;

// Each page's title is fitted again, as its text changes from page to page, and the folder is not
// shown selected (an edit's selection is Windows blue, whatever the style).
procedure CurPageChanged(CurPageID: Integer);
begin
  Header(WizardForm.PageNameLabel, WizardForm.PageDescriptionLabel, True);
  if CurPageID = wpSelectDir then
    WizardForm.DirEdit.SelStart := Length(WizardForm.DirEdit.Text);
end;

procedure InitializeUninstallProgressForm;
begin
  Header(UninstallProgressForm.PageNameLabel, UninstallProgressForm.PageDescriptionLabel, False);
  LauncherBar(UninstallProgressForm);
end;
