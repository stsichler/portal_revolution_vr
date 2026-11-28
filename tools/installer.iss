; Setup of the Portal Revolution VR mod, built by tools\package-release.ps1 with Inno Setup 6:
;   ISCC /DStage=<stage folder> /DVersion=<version> /O<output folder> tools\installer.iss
; The stage folder is the one the ZIP is made of, so the file list exists only once.
; The setup keeps VR\config.txt unless the player has it replaced: the mod makes and completes it
; from VR\config.default.txt.

#ifndef Stage
  #error Define Stage, the stage folder of package-release.ps1
#endif
#ifndef Version
  #define Version "dev"
#endif

[Setup]
; Never change the AppId: updates and the uninstaller find the installation by it.
AppId={{2B7B446F-DAEA-4E32-A6D4-E19EAA3ACE99}
AppName=Portal: Revolution VR Mod
AppVersion={#Version}
AppPublisher=Stefan Sichler
AppPublisherURL=https://github.com/stsichler/portal_revolution_vr
AppSupportURL=https://github.com/stsichler/portal_revolution_vr/issues
UninstallDisplayName=Portal: Revolution VR Mod {#Version}
DefaultDirName={code:GameDir}
AppendDefaultDirName=no
DirExistsWarning=no
DisableDirPage=no
DisableProgramGroupPage=yes
; For the banner, see InitializeWizard.
DisableWelcomePage=no
UninstallFilesDir={app}\VR\uninstall
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; The running game holds bin\win64\d3d11.dll.
CloseApplications=yes
RestartApplications=no
ShowLanguageDialog=auto
OutputBaseFilename=PortalRevolutionVR-{#Version}-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "brazilian"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "french"; MessagesFile: "compiler:Languages\French.isl"
Name: "german"; MessagesFile: "compiler:Languages\German.isl"
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "turkish"; MessagesFile: "compiler:Languages\Turkish.isl"
Name: "ukrainian"; MessagesFile: "compiler:Languages\Ukrainian.isl"

[Messages]
; Instead of the advice to close all other applications.
english.WelcomeLabel2=This installs [name/ver] into the game's folder.
brazilian.WelcomeLabel2=Isto instala [name/ver] na pasta do jogo.
french.WelcomeLabel2=Ceci installe [name/ver] dans le dossier du jeu.
german.WelcomeLabel2=Das installiert [name/ver] in den Ordner des Spiels.
italian.WelcomeLabel2=Questo installa [name/ver] nella cartella del gioco.
japanese.WelcomeLabel2=[name/ver] をゲームのフォルダーにインストールします。
russian.WelcomeLabel2=[name/ver] будет установлен в папку игры.
spanish.WelcomeLabel2=Esto instala [name/ver] en la carpeta del juego.
turkish.WelcomeLabel2=[name/ver] oyunun klasörüne kurulacak.
ukrainian.WelcomeLabel2=[name/ver] буде встановлено в теку гри.

[CustomMessages]
english.NotGameFolder=This is not the folder of Portal: Revolution. Select the game's folder, usually ...\steamapps\common\Portal Revolution.
english.DeleteConfig=Also delete your VR settings (VR\config.txt)?
brazilian.NotGameFolder=Esta não é a pasta de Portal: Revolution. Escolha a pasta do jogo, normalmente ...\steamapps\common\Portal Revolution.
brazilian.DeleteConfig=Excluir também suas configurações de VR (VR\config.txt)?
french.NotGameFolder=Ce n'est pas le dossier de Portal: Revolution. Choisissez le dossier du jeu, en général ...\steamapps\common\Portal Revolution.
french.DeleteConfig=Supprimer aussi vos réglages VR (VR\config.txt) ?
german.NotGameFolder=Das ist nicht der Ordner von Portal: Revolution. Wähle den Ordner des Spiels, meist ...\steamapps\common\Portal Revolution.
german.DeleteConfig=Auch deine VR-Einstellungen löschen (VR\config.txt)?
italian.NotGameFolder=Questa non è la cartella di Portal: Revolution. Scegli la cartella del gioco, di solito ...\steamapps\common\Portal Revolution.
italian.DeleteConfig=Eliminare anche le tue impostazioni VR (VR\config.txt)?
japanese.NotGameFolder=これは Portal: Revolution のフォルダーではありません。ゲームのフォルダー（通常は ...\steamapps\common\Portal Revolution）を選んでください。
japanese.DeleteConfig=VR の設定 (VR\config.txt) も削除しますか？
russian.NotGameFolder=Это не папка Portal: Revolution. Выберите папку игры, обычно ...\steamapps\common\Portal Revolution.
russian.DeleteConfig=Удалить также ваши настройки VR (VR\config.txt)?
spanish.NotGameFolder=Esta no es la carpeta de Portal: Revolution. Elige la carpeta del juego, normalmente ...\steamapps\common\Portal Revolution.
spanish.DeleteConfig=¿Borrar también tus ajustes de VR (VR\config.txt)?
turkish.NotGameFolder=Bu, Portal: Revolution klasörü değil. Oyunun klasörünü seçin, genellikle ...\steamapps\common\Portal Revolution.
turkish.DeleteConfig=VR ayarlarınız da silinsin mi (VR\config.txt)?
ukrainian.NotGameFolder=Це не тека Portal: Revolution. Виберіть теку гри, зазвичай ...\steamapps\common\Portal Revolution.
ukrainian.DeleteConfig=Також видалити ваші налаштування VR (VR\config.txt)?
; The folder page when the game wasn't found.
english.GameNotFound=Portal: Revolution wasn't found. Click Browse and select its folder, usually ...\steamapps\common\Portal Revolution.
brazilian.GameNotFound=Portal: Revolution não foi encontrado. Clique em Procurar e escolha a pasta do jogo, normalmente ...\steamapps\common\Portal Revolution.
french.GameNotFound=Portal: Revolution est introuvable. Cliquez sur Parcourir et choisissez son dossier, en général ...\steamapps\common\Portal Revolution.
german.GameNotFound=Portal: Revolution wurde nicht gefunden. Klicke auf Durchsuchen und wähle seinen Ordner, meist ...\steamapps\common\Portal Revolution.
italian.GameNotFound=Portal: Revolution non è stato trovato. Fai clic su Sfoglia e scegli la sua cartella, di solito ...\steamapps\common\Portal Revolution.
japanese.GameNotFound=Portal: Revolution が見つかりません。[参照] をクリックしてゲームのフォルダー（通常は ...\steamapps\common\Portal Revolution）を選んでください。
russian.GameNotFound=Portal: Revolution не найдена. Нажмите «Обзор» и выберите папку игры, обычно ...\steamapps\common\Portal Revolution.
spanish.GameNotFound=No se encontró Portal: Revolution. Haz clic en Examinar y elige su carpeta, normalmente ...\steamapps\common\Portal Revolution.
turkish.GameNotFound=Portal: Revolution bulunamadı. Göz at'a tıklayıp oyunun klasörünü seçin, genellikle ...\steamapps\common\Portal Revolution.
ukrainian.GameNotFound=Portal: Revolution не знайдено. Натисніть «Огляд» і виберіть теку гри, зазвичай ...\steamapps\common\Portal Revolution.
; The question at the start when the game wasn't found.
english.GameMissing=Portal: Revolution wasn't found.
english.GameMissingText=Install the game with Steam first, then run this setup again.
english.GameMissingStore=&Open the Steam page
english.GameMissingContinue=&Continue anyway%nThe game is installed: select its folder yourself.
brazilian.GameMissing=Portal: Revolution não foi encontrado.
brazilian.GameMissingText=Instale primeiro o jogo pela Steam e depois execute este instalador novamente.
brazilian.GameMissingStore=&Abrir a página da Steam
brazilian.GameMissingContinue=&Continuar mesmo assim%nO jogo está instalado: escolha você mesmo a pasta dele.
french.GameMissing=Portal: Revolution est introuvable.
french.GameMissingText=Installez d'abord le jeu avec Steam, puis relancez ce programme d'installation.
french.GameMissingStore=&Ouvrir la page Steam
french.GameMissingContinue=&Continuer quand même%nLe jeu est installé : choisissez vous-même son dossier.
german.GameMissing=Portal: Revolution wurde nicht gefunden.
german.GameMissingText=Installiere zuerst das Spiel über Steam und starte dieses Setup dann erneut.
german.GameMissingStore=Steam-Seite &öffnen
german.GameMissingContinue=&Trotzdem fortfahren%nDas Spiel ist installiert: wähle seinen Ordner selbst.
italian.GameMissing=Portal: Revolution non è stato trovato.
italian.GameMissingText=Installa prima il gioco con Steam, poi avvia di nuovo questo setup.
italian.GameMissingStore=&Apri la pagina di Steam
italian.GameMissingContinue=&Continua comunque%nIl gioco è installato: scegli tu la sua cartella.
japanese.GameMissing=Portal: Revolution が見つかりません。
japanese.GameMissingText=先に Steam でゲームをインストールしてから、このセットアップをもう一度実行してください。
japanese.GameMissingStore=Steam のページを開く(&O)
japanese.GameMissingContinue=このまま続ける(&C)%nゲームはインストール済みです。フォルダーを自分で選んでください。
russian.GameMissing=Portal: Revolution не найдена.
russian.GameMissingText=Сначала установите игру через Steam, затем снова запустите установщик.
russian.GameMissingStore=&Открыть страницу в Steam
russian.GameMissingContinue=&Всё равно продолжить%nИгра установлена: выберите её папку вручную.
spanish.GameMissing=No se encontró Portal: Revolution.
spanish.GameMissingText=Instala primero el juego con Steam y luego vuelve a ejecutar este instalador.
spanish.GameMissingStore=&Abrir la página de Steam
spanish.GameMissingContinue=&Continuar de todos modos%nEl juego está instalado: elige tú su carpeta.
turkish.GameMissing=Portal: Revolution bulunamadı.
turkish.GameMissingText=Önce oyunu Steam ile kurun, ardından bu kurulumu yeniden çalıştırın.
turkish.GameMissingStore=Steam sayfasını &aç
turkish.GameMissingContinue=Yine de &devam et%nOyun kurulu: klasörünü kendiniz seçin.
ukrainian.GameMissing=Portal: Revolution не знайдено.
ukrainian.GameMissingText=Спочатку встановіть гру через Steam, а потім знову запустіть інсталятор.
ukrainian.GameMissingStore=&Відкрити сторінку в Steam
ukrainian.GameMissingContinue=&Усе одно продовжити%nГру встановлено: виберіть її теку вручну.
; The page when VR\config.txt already exists.
english.ConfigCaption=VR settings
english.ConfigDescription=You already have VR settings (VR\config.txt).
english.ConfigKeep=Keep my settings
english.ConfigReplace=Replace them with the defaults
brazilian.ConfigCaption=Configurações de VR
brazilian.ConfigDescription=Você já tem configurações de VR (VR\config.txt).
brazilian.ConfigKeep=Manter minhas configurações
brazilian.ConfigReplace=Substituí-las pelas configurações padrão
french.ConfigCaption=Réglages VR
french.ConfigDescription=Vous avez déjà des réglages VR (VR\config.txt).
french.ConfigKeep=Garder mes réglages
french.ConfigReplace=Les remplacer par les réglages par défaut
german.ConfigCaption=VR-Einstellungen
german.ConfigDescription=Du hast schon VR-Einstellungen (VR\config.txt).
german.ConfigKeep=Meine Einstellungen behalten
german.ConfigReplace=Durch die Standardeinstellungen ersetzen
italian.ConfigCaption=Impostazioni VR
italian.ConfigDescription=Hai già delle impostazioni VR (VR\config.txt).
italian.ConfigKeep=Mantieni le mie impostazioni
italian.ConfigReplace=Sostituiscile con quelle predefinite
japanese.ConfigCaption=VR の設定
japanese.ConfigDescription=VR の設定 (VR\config.txt) がすでにあります。
japanese.ConfigKeep=今の設定を残す
japanese.ConfigReplace=既定の設定に置き換える
russian.ConfigCaption=Настройки VR
russian.ConfigDescription=У вас уже есть настройки VR (VR\config.txt).
russian.ConfigKeep=Сохранить мои настройки
russian.ConfigReplace=Заменить настройками по умолчанию
spanish.ConfigCaption=Ajustes de VR
spanish.ConfigDescription=Ya tienes ajustes de VR (VR\config.txt).
spanish.ConfigKeep=Conservar mis ajustes
spanish.ConfigReplace=Reemplazarlos por los predeterminados
turkish.ConfigCaption=VR ayarları
turkish.ConfigDescription=Zaten VR ayarlarınız var (VR\config.txt).
turkish.ConfigKeep=Ayarlarımı koru
turkish.ConfigReplace=Varsayılan ayarlarla değiştir
ukrainian.ConfigCaption=Налаштування VR
ukrainian.ConfigDescription=У вас уже є налаштування VR (VR\config.txt).
ukrainian.ConfigKeep=Залишити мої налаштування
ukrainian.ConfigReplace=Замінити типовими налаштуваннями

[InstallDelete]
; The uninstaller of the ZIP; this setup brings its own.
Type: files; Name: "{app}\Uninstall_Portal_Revolution_VR_Mod.cmd"

[Files]
Source: "{#Stage}\*"; DestDir: "{app}"; Excludes: "\Uninstall_Portal_Revolution_VR_Mod.cmd"; Flags: recursesubdirs ignoreversion
; The README's logo, the banner of the welcome page.
Source: "..\imgs\logo.png"; Flags: dontcopy

[UninstallDelete]
; Written by the mod. Folders of a ZIP installation aren't the setup's, so they are named here.
Type: filesandordirs; Name: "{app}\VR\override"
Type: filesandordirs; Name: "{app}\VR\licenses"
Type: filesandordirs; Name: "{app}\VR\localization"
Type: filesandordirs; Name: "{app}\VR\SteamVRActionManifest"
Type: files; Name: "{app}\prvr_debug.log"
Type: files; Name: "{app}\prvr_frame_*.txt"

[Code]
const
  SteamAppKey = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 601360';
  StoreURL = 'https://store.steampowered.com/app/601360/';
  // This setup's own uninstall entry, from the AppId.
  SetupKey = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{2B7B446F-DAEA-4E32-A6D4-E19EAA3ACE99}_is1';

var
  ConfigPage: TInputOptionWizardPage;

function ConfigFile: String;
begin
  Result := AddBackslash(WizardDirValue) + 'VR\config.txt';
end;

// The welcome page shows the logo over its full width instead of the side image, the text below.
procedure ShowBanner;
var
  Banner: TBitmapImage;
  Page: TNewNotebookPage;
  Margin: Integer;
begin
  Page := WizardForm.WelcomePage;
  ExtractTemporaryFile('logo.png');
  Banner := TBitmapImage.Create(WizardForm);
  Banner.Parent := Page;
  Banner.PngImage.LoadFromFile(ExpandConstant('{tmp}\logo.png'));
  Banner.Stretch := True;
  Banner.SetBounds(0, 0, Page.ClientWidth, Page.ClientWidth * Banner.PngImage.Height div Banner.PngImage.Width);
  WizardForm.WizardBitmapImage.Visible := False;
  // The margin the text has on the right.
  Margin := Page.ClientWidth - WizardForm.WelcomeLabel1.Left - WizardForm.WelcomeLabel1.Width;
  WizardForm.WelcomeLabel1.SetBounds(Margin, Banner.Height + Margin, Page.ClientWidth - 2 * Margin, WizardForm.WelcomeLabel1.Height);
  WizardForm.AdjustLabelHeight(WizardForm.WelcomeLabel1);
  WizardForm.WelcomeLabel2.SetBounds(Margin, WizardForm.WelcomeLabel1.Top + WizardForm.WelcomeLabel1.Height,
    Page.ClientWidth - 2 * Margin, Page.ClientHeight - WizardForm.WelcomeLabel1.Top - WizardForm.WelcomeLabel1.Height);
end;

// Keep or replace an existing config; keep is preselected and the choice of silent setups.
procedure InitializeWizard;
begin
  ShowBanner;
  ConfigPage := CreateInputOptionPage(wpSelectDir, CustomMessage('ConfigCaption'),
    CustomMessage('ConfigDescription'), '', True, False);
  ConfigPage.Add(CustomMessage('ConfigKeep'));
  ConfigPage.Add(CustomMessage('ConfigReplace'));
  ConfigPage.SelectedValueIndex := 0;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = ConfigPage.ID) and not FileExists(ConfigFile);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and FileExists(ConfigFile) and (ConfigPage.SelectedValueIndex = 1) then
    FileCopy(AddBackslash(WizardDirValue) + 'VR\config.default.txt', ConfigFile, False);
end;

function IsGameDir(Dir: String): Boolean;
begin
  Result := FileExists(AddBackslash(Dir) + 'bin\win64\engine.dll') and DirExists(AddBackslash(Dir) + 'revolution');
end;

// Value of a line "Key" "Value" of a Steam .vdf or .acf file.
function VdfValue(Line, Key: String; var Value: String): Boolean;
begin
  Line := Trim(Line);
  Key := '"' + Key + '"';
  Result := CompareText(Copy(Line, 1, Length(Key)), Key) = 0;
  if Result then
  begin
    Value := Trim(Copy(Line, Length(Key) + 1, MaxInt));
    Result := (Length(Value) >= 2) and (Value[1] = '"') and (Value[Length(Value)] = '"');
    if Result then
    begin
      Value := Copy(Value, 2, Length(Value) - 2);
      StringChangeEx(Value, '\\', '\', True);
    end;
  end;
end;

// The game's folder in a Steam library, from its appmanifest; '' if it isn't there.
function GameDirInLibrary(LibDir: String): String;
var
  Lines: TArrayOfString;
  I: Integer;
  InstallDir: String;
begin
  Result := '';
  LibDir := AddBackslash(LibDir) + 'steamapps\';
  if LoadStringsFromFile(LibDir + 'appmanifest_601360.acf', Lines) then
    for I := 0 to GetArrayLength(Lines) - 1 do
      if VdfValue(Lines[I], 'installdir', InstallDir) and IsGameDir(LibDir + 'common\' + InstallDir) then
      begin
        Result := LibDir + 'common\' + InstallDir;
        Exit;
      end;
end;

// Searches Steam's own folder and every library of steamapps\libraryfolders.vdf.
function SteamGameDir: String;
var
  Steam, LibDir: String;
  Lines: TArrayOfString;
  I: Integer;
begin
  Result := '';
  if not RegQueryStringValue(HKLM32, 'SOFTWARE\Valve\Steam', 'InstallPath', Steam) and
     not RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', Steam) then
    Exit;
  StringChangeEx(Steam, '/', '\', True);
  Result := GameDirInLibrary(Steam);
  if (Result = '') and LoadStringsFromFile(AddBackslash(Steam) + 'steamapps\libraryfolders.vdf', Lines) then
    for I := 0 to GetArrayLength(Lines) - 1 do
      if VdfValue(Lines[I], 'path', LibDir) then
      begin
        Result := GameDirInLibrary(LibDir);
        if Result <> '' then
          Exit;
      end;
end;

// Steam's libraries first: the uninstall entry can be missing or outdated.
function GameDir(Param: String): String;
begin
  Result := SteamGameDir;
  if (Result = '') and
     not RegQueryStringValue(HKLM64, SteamAppKey, 'InstallLocation', Result) and
     not RegQueryStringValue(HKLM32, SteamAppKey, 'InstallLocation', Result) then
    Result := ExpandConstant('{commonpf32}\Steam\steamapps\common\Portal Revolution');
end;

// Not found, not even where the mod was installed before: install the game first?
function InitializeSetup: Boolean;
var
  PrevDir: String;
  ErrorCode: Integer;
begin
  Result := True;
  if WizardSilent or IsGameDir(GameDir('')) or
     (RegQueryStringValue(HKLM64, SetupKey, 'Inno Setup: App Path', PrevDir) and IsGameDir(PrevDir)) then
    Exit;
  case TaskDialogMsgBox(CustomMessage('GameMissing'), CustomMessage('GameMissingText'), mbInformation,
      MB_YESNOCANCEL, [CustomMessage('GameMissingStore'), CustomMessage('GameMissingContinue')], 0) of
    IDYES:
      begin
        ShellExecAsOriginalUser('open', StoreURL, '', '', SW_SHOWNORMAL, ewNoWait, ErrorCode);
        Result := False;
      end;
    IDCANCEL:
      Result := False;
  end;
end;

// Not found: the folder page says to select the game's folder.
procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpSelectDir) and not IsGameDir(WizardDirValue) then
    WizardForm.SelectDirBrowseLabel.Caption := CustomMessage('GameNotFound');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and not IsGameDir(WizardDirValue) then
  begin
    MsgBox(CustomMessage('NotGameFolder'), mbError, MB_OK);
    Result := False;
  end;
end;

var
  DeleteSettings: Boolean;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    DeleteSettings := SuppressibleMsgBox(CustomMessage('DeleteConfig'), mbConfirmation,
      MB_YESNO or MB_DEFBUTTON2, IDNO) = IDYES;
  // With the settings the whole VR\ goes; otherwise VR\ stays with config.txt.
  if CurUninstallStep = usPostUninstall then
    if DeleteSettings then
      DelTree(ExpandConstant('{app}\VR'), True, True, True)
    else
      RemoveDir(ExpandConstant('{app}\VR\uninstall'));
end;
