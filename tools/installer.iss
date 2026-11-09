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
AppName=Portal Revolution VR
AppVersion={#Version}
AppPublisher=Stefan Sichler
AppPublisherURL=https://github.com/stsichler/portal_revolution_vr
AppSupportURL=https://github.com/stsichler/portal_revolution_vr/issues
UninstallDisplayName=Portal Revolution VR {#Version}
DefaultDirName={code:GameDir}
AppendDefaultDirName=no
DirExistsWarning=no
DisableDirPage=no
DisableProgramGroupPage=yes
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

var
  ConfigPage: TInputOptionWizardPage;

function ConfigFile: String;
begin
  Result := AddBackslash(WizardDirValue) + 'VR\config.txt';
end;

// Keep or replace an existing config; keep is preselected and the choice of silent setups.
procedure InitializeWizard;
begin
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

function GameDir(Param: String): String;
begin
  if not RegQueryStringValue(HKLM64, SteamAppKey, 'InstallLocation', Result) and
     not RegQueryStringValue(HKLM32, SteamAppKey, 'InstallLocation', Result) then
    Result := ExpandConstant('{commonpf32}\Steam\steamapps\common\Portal Revolution');
end;

function IsGameDir(Dir: String): Boolean;
begin
  Result := FileExists(AddBackslash(Dir) + 'bin\win64\engine.dll') and DirExists(AddBackslash(Dir) + 'revolution');
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

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if (CurUninstallStep = usUninstall) and
     (SuppressibleMsgBox(CustomMessage('DeleteConfig'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDNO) = IDYES) then
    DeleteFile(ExpandConstant('{app}\VR\config.txt'));
  // VR\ stays if it still has the config.
  if CurUninstallStep = usPostUninstall then
  begin
    RemoveDir(ExpandConstant('{app}\VR\uninstall'));
    RemoveDir(ExpandConstant('{app}\VR'));
  end;
end;
