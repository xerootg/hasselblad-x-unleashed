# X2D II (fw v1.3.16.2) swipe-left settings menu "GAME" injection-point investigation

Date: 2026-09-22 ｜ Target: `system_decrypted/bin/camera-gui` (64,082,728B, aarch64, full symbols, static Qt 6.8.2)
Method: full qrc tree extraction (core/qrc_dump.py, 1112 files, 0 errors) + WSL objdump disassembly + .tmpwork/camera-gui.nm symbol table + verification against embedded .qm translations
Inputs: REPORT_O1 (QML loading chain/AOT), REPORT_O3 (menu tables/drive modes), REPORT_P (LD_PRELOAD hardware precedents), analysis/gui_secret_menu.md

---

## 0. Conclusion in one sentence

**The settings menu's 11 categories are neither defined in QML nor driven by translation files—they come from a static C++ table in menux2d.cpp (`Menu::topMenus()`), fed into the `MainScreen.qml` grid through the C++ singleton `FavoriteModel` (QAbstractListModel); QML cannot append to the C++ model, but `MainScreen_grid.model` and the page itself can be replaced/overlaid at runtime from the injected wrapper (the same technique as P2 driveModeModel), while the launch path `System.doRun_debug_script(1..4)` → `/system/bin/custom_debug_%1.sh` is already natively supported by the official custom-button dropdown. Recommendation = existing modqml.so thin wrapper + runtime GAME button overlay in MainScreen's empty 12th tile + doRun_debug_script(N); full mirroring (changing appPrefix) is the upgrade path for a pixel-perfect native result.**

---

## 1. Source of the settings menu list (answer to question 1)

### 1.1 Rendering chain (fully verified)

```
main.qml (qrc, 59-line thin wrapper)
 └ MainContent.qml
   └ controlscreen/ControlDrawer.qml          ← Swipe-left container: HblListView, 2 pages
       ├ [0] ControlScreen.qml (viewfinder control screen)
       └ [1] Constants.appPrefix + "mainmenu/MainScreen.qml"   ← Page revealed by swiping left
            └ Components.CustomGridView (objectName "MainScreen_grid")
                model: viewModel.favoriteModel          ← C++ QML singleton
                delegate: FramedImage { text: qsTranslate(labelContext, label);
                                        source: iconSrc; onClicked→gotClick() }
                └ gotClick() → root.loadSubMenuItem(menuName,...)   [signal]
                     → ControlDrawer: (item as MainScreen).loadSubMenuItem
                       .connect(mainMenu.loadSubmenu)   (connected in onLoaded)
                     → MainMenu.loadSubMenuItem(menuName)
                        ├ gate: SubMenu.menuRw(menuName)
                        ├ viewModel.menu.file = SubMenu.menuFile(menuName)  ← C++ map looks up QML file
                        └ menu_loader.source = file → SettingsGeneric.qml
                            └ populateModel(): list.model = SubMenu.getModel(level)
```

- **MainScreen.qml** = category grid page (not "favorites"—`FavoriteModel::init()` @0x16f1f80 calls
  addItem for **all** valid top-level menus from `Menu::topMenus()`, then sets each itemEnabled according to conditionValid(avail/rw)).
- **SettingsGeneric.qml** = settings list page opened by tapping a category; list model = C++ `SubMenu.getModel(level)` (SubMenuModel/FilteredSubMenuModel, menumodel module, qmldir `prefer :/qt/qml/...`, registered C++ singleton).

### 1.2 Category data is in C++, not QML

| Evidence | Address/file |
|---|---|
| `Menu::topMenus()` facade → `MenuX2d::topMenus()` returns a statically initialized QVector | 0x16f4b68 / 0x1706b40 (returns bss 0x2b4ce70) |
| `MenuX2d::menuFile(QString)`: QMap<QString,MenuInfo> looks up name→QML file; **unknown name miss → returns empty QString** (0x1706d9c `stp xzr,xzr`) | 0x1706ca8 |
| Top-level Submenu construction sequence (`Submenu::Submenu(QString&, E_MenuOption, bool, E_MenuRW, E_MenuAvail)` @0x1703b08) | menux2d.cpp static initialization 0x395724–0x396c34, etc. |
| `FavoriteModel` (C++ QML singleton, favoritemodel.cpp) init/reloadModel/addItem | 0x16f1f80 / 0x16f1f48 / 0x16f2530 |
| menuName + icon literals for the 11 categories (UTF-16 pairs in .rodata) | 0x1cedb00–0x1cee566 region |
| Chinese category labels = embedded .qm translations (`qrc:/translations/camera_zh_CN.qm`; all labels corresponding to "Crop Modes/Power/Flash/Stabilization…" found); QML uses `qsTranslate(labelContext,label)` | t02235d0d tree |

The 11 top-level categories (menuName ↔ icon ↔ user-visible label):

| menuName | icon | Chinese label (translated) |
|---|---|---|
| exposureMenu | ic_menu_exposure | Exposure |
| focusMenu | ic_menu_focus | Focus |
| qualityMenu | ic_menu_quality | Image Quality |
| cropModesMenu | ic_menu_crop_modes | Crop Modes |
| flashMenu | ic_menu_flash | Flash |
| displayMenu | ic_menu_display | Display |
| powerMenu | ic_menu_power | Power |
| storageMenu | ic_menu_storage | Storage |
| ibisMenu | ic_menu_ibis | Stabilization |
| connectionMenu | ic_menu_connection | Connection |
| generalMenu | ic_menu_general | General |

Other nested submenu names (strings in the same region, for reference): aboutMenu/cameraBodyMenu/customButtonsMenu/customOverlaysMenu/dateTimeMenu/developerMenu/languageMenu/lensSelectionMenu/profilesMenu/serviceMenu(General→Service, verified by the zh translation corresponding to "General - Service - Sensor Calibration")/soundMenu/spiritLevelMenu/touchMenu/wifiMenu/usageMenu.

**Complete list of settings-menu-related files in the main QML tree** (under qrc:/qt/qml/app/qml/, dumped to core/qrc_dump/t021600e8/):
mainmenu/ 32 files: MainMenu.qml(240 lines, slide-in loader) MenuSwipe.qml(143 lines, dragging) MainScreen.qml(177 lines, **category grid**) MainMenuViewModel.qml(359 lines) MenuData.qml MenuBase.qml SubMenuBase.qml SettingsGeneric.qml(518 lines, **list page**) MenuListView.qml MenuHeader.qml MenuSubdialog.qml ListSelectorSettings.qml SettingSlider.qml MenuBoolSelector.qml CustomButtonsView.qml(459) CustomButtonsItem.qml(297) MyLenses.qml ProfilesView.qml DateTime.qml TimeZoneView.qml LicenseView.qml VibrationsView.qml FirstTimeGuide*.qml + delegates/(SubmenuDelegate/ButtonDelegate/DropdownDelegate/SliderDelegate/DateTimeDelegate/ValueSwitchDelegate) + popups/(MenuPopup/ConfirmResetAllSettings/DiskFormat/ProfileEdit/EvfDiopterSelector/PopoverCombinationLock/RecalibrateSensor).
Supporting files: components/MenuItemModel.qml, controlscreen/ControlDrawer.qml (swipe-left container+MainScreen loading), viewmodels/StateMachineViewModel.qml (custom-button handling).

### 1.3 Why MainScreen.qml cannot simply be edited in qrc

O1 §4 established that qmlcachegen AOT compilation units always take precedence for URLs fully matching the `qrc:` scheme + path (sourceTimestamp=0 is always accepted)—replacing QML text in qrc at the same offset/location is invisible to the engine. The only viable options are (a) loading file: URLs (scheme mismatch bypasses AOT) or (b) modifying the object tree at runtime from external QML.

---

## 2. Full qrc extraction (answer to question 2, completed)

- **Tool**: `core/qrc_dump.py` (self-contained). Enumerates all `BL qRegisterResourceData(0xe90980)` call sites in .text, traces back ADRP/ADD to decode the three (TREE, NAMES, DATA) pointers → traverses 22-byte big-endian node trees → decompresses zlib → writes files using their actual paths.
- **Evidence-based rcc format corrections** (this build; differences from upstream Qt are noted):
  - Nodes are 22B: `@0 u32 name_off | @4 u16 flags(bit1=Directory, bit0=zlib) | @6 u32 Dir:childcount/File:always1 | @10 u32 Dir:firstchild/**File:payload_off** | @14 u32 timestamp | @18 u32 checksum`. (The old comment in .tmpwork/o1/rcc2.py incorrectly places payload_off at @6; @6 is always 1 for file nodes.)
  - names entries: `[u16 len][u32 hash][UTF-16BE]`; payload entries: plain=`[u32 size][data]`, zlib=`[u32 comp][u32 unc][zlib]`.
  - The first path component is duplicated (rcc mount root); URL = `qrc:/` + path with the first component removed.
- **Output**: `core/qrc_dump/`—**72 trees / 1112 files / 0 errors** (381 QML + 459 SVG + 66 qmldir + 10 .qm translations + qsb shaders, etc., 9.7MB); `manifest.json` contains each file's payload offset/size/compression flag. Main QML tree = `t021600e8/qt/qml/app/qml/` (346 files), icon tree `t022cbeb6/icons/` (470), translations `t02235d0d/translations/`.
- **Validation**: `t021600e8/qt/qml/app/qml/main.qml` matches `.tmpwork/o1/main.qml` (extracted for the O1 report) byte for byte; all 346 main-tree zlib/plain files decompressed successfully and passed length checks.
- These are the assets for the "full on-disk deployment" approach: the entire app QML tree is ready to mirror directly to /tmp/modqml/.

---

## 3. UI entry points for System.run_debug_script (answer to question 3)

### 3.1 Complete call chain

```
QML: System.doRun_debug_script(N)                       ← com.hasselblad.proxies System singleton
  (StateMachineViewModel.qml L428-439; SystemProxyDbus::doRun_debug_script(int) @0x1067dc0 W symbol;
   DBus slot SystemProxyDbus::run_debug_script(int,int,QObject*) @0x1092ef8; P1 mod_main6.qml also verified
   that the System.run_debug_script(5) form is callable)
 → DBus com.hasselblad.system → camera-system SystemObjectImpl::doRun_debug_script
 → ScriptHandler::runScript(int) (@camera-system 0xfce28)
 → kScriptPathTemplate = **"/system/bin/custom_debug_%1.sh"**   ← Verified as UTF-16 in the camera-system binary
    (Adjacent strings include "Start debug script:"/"Invalid script id:"/"Script already running:")
```

### 3.2 Native UI entry = custom-button dropdown (no injection, officially supported)

- **Only QML call site**: `viewmodels/StateMachineViewModel.qml` handleCustomButton():
  `case E_UserButtonFunction_DebugScript1..4: System.doRun_debug_script(1..4)`.
- **The dropdown includes this option**: E_UserButtonFunction value-table labels "Debug Script 1"–"Debug Script 4"
  (UTF-16 @0x1d16590/0x1d165ae/0x1d165cc/0x1d165ea, adjacent to custom-button function labels such as "Live View AF Dump"/"Screenshot").
  Menu table user_button_{custom_front,custom_rear,wheel_click_front,wheel_click_rear,joystick_click,m,iso_wb}_function
  (menux2d 0x3a5fac–0x3a8000, labels such as "Front Custom Btn.").
  → Native path: Settings→General→Custom Buttons→assign "Debug Script 1" to a button→press that button = execute /system/bin/custom_debug_1.sh.
- **Note**: scripts run in the **camera-system** (root) process, not the GUI; /tmp/x2nes needs the weston/WAYLAND_DISPLAY environment to display output—the script must handle that (see REPORT_P §6.2 for lessons about screen double buffering/eagle-shell).

### 3.3 Hidden engineering/factory menu switch investigation (conclusion: none exposes an "entry point")

| Switch | Type | Conclusion |
|---|---|---|
| `GuiConfig.showSecretMenuItems` | Nonpersistent runtime bool (default false); only write entry = QML `GuiConfig.showSecretMenuItems = true` (gui_secret_menu.md; gated menux2d items = FW Update Retry/Factory Reset/Clear Sensor Calibration/Grip FW Retry buttons in the Service menu's "Additional Functions" section + Demo Mode section header) | **No game/script entry**; could be an Easter-egg switch in the GAME panel |
| `System.debug_options` (E_DebugOption 27 bits) | DBus-writable bitmask | Enables only AF/HDR/Histogram and other HUD overlays (O3 §3.1), not menu structure changes |
| E_MenuAvail_DeveloperCamera=8 | Condition enum | Used by **0 items** in menux2d (CFV backs only) → no engineering menu to unlock on this body |
| E_MenuRW=32 NotImageProcessing | serviceMenu rw condition (0x396864 w4=0x20) | Not a hidden switch; only disables items during image processing |
| Engineering switches of the form persist.*/hbl.* | — | No evidence in camera-gui that such properties control menu visibility |
| developerMenu/serviceMenu | Nested submenus under General | Normally accessible (Service=sensor calibration, etc.); developerMenu contains a list of E_DebugOption switches |

**No run_debug_script entry is more native than custom buttons**; the settings menu has no script-execution items.

---

## 4. Injection approach evaluation (answer to question 4)

### Approach A: full qrc mirror + appPrefix replacement (pixel-perfect native)

Method: on top of modqml.so's existing BSS patch (0x2b054b8 → file:///tmp/modqml/m + mmain.qml, verified in P1 v4),
**also patch {ptr,size} in the CommonConstants copy at `0x2b494e8` to point to a RW zero-filled buffer containing "file:///tmp/modqml/" (same length, 20 characters)**.
Effect: all URLs constructed in QML as `Constants.appPrefix + "..."` (ControlDrawer→MainScreen.qml, SettingsGeneric, popovers) load from disk; mirror the entire `core/qrc_dump/t021600e8/qt/qml/app/qml/` tree (346 files) to /tmp/modqml/, and edit MainScreen.qml to add a GAME tile.

- Advantages: GAME becomes a real grid item (same delegate, translation, and full focus/joystick-navigation compatibility); files not mirrored/edited continue using qrc+AOT unaffected (hardcoded qrc: URLs still resolve to original resources; mixed use is safe).
- Effort: ★★ (mirroring=copy 3MB; patch=add 2 lines to modqml.so; edit 1 QML file).
- Risks:
  1. **QString::append in-place write trap** (confirmed in P1 v3): append("main.qml") modifies the app_main copy—a v4-style RW zero-filled buffer is required; the CommonConstants copy is only read for concatenation (+), so risk is low, but a RW buffer is still safest.
  2. Whether the svgimageprovider copy at 0x2b05b10 uses appPrefix to construct image://svg paths is unverified—if icons fail, apply the same patch + mirror the icons tree (dump ready). Also note the separate "qrc:///icons/" string at 0x1cee566; icons likely use an independent prefix and remain unaffected.
  3. file: loads lose AOT → runtime compilation (P1 proved this works; during iteration add QML_DISABLE_DISK_CACHE=1 + rm /data/qmlcache).
  4. Each GUI restart requires ctl.stop/start weston (eagle-shell outputs are not reclaimed, REPORT_P §6.2).
- Prerequisites ready: full qrc_dump tree + manifest.

### Approach B: native custom button + custom_debug_N.sh (no injection)

Method: write no QML. Settings→General→Custom Buttons→"Rear Custom Btn."= "Debug Script 1"; deploy /system/bin/custom_debug_1.sh (exec /tmp/x2nes or a launch wrapper).
- Advantages: ★ effort, no modifications, official hook, stable.
- Drawbacks: **no "GAME" menu item** (button-triggered, not visible in the swipe-left menu); /system may be read-only (deploying the script requires remounting or a /tmp+symlink approach—if custom_debug_1.sh must reside in /system/bin, feasibility depends on partition state; report P did not verify script deployment).
- Role: fallback/debug channel, and the actual execution backend when the GAME item is tapped (both approaches share the chain in 3.1).

### Approach C (recommended): existing thin wrapper + runtime GAME tile overlay + doRun_debug_script

Method: reuse the P1/P2-verified path (modqml.so v4 + /tmp/modqml/mmain.qml thin wrapper; everything else remains qrc+AOT). A Timer in the wrapper delays traversal by objectName to find `MainScreen_root` / `MainScreen_grid`, then overlays a GAME button on the page:
- **C1 (preferred, ~30 lines)**: use `Qt.createComponent(qrc:FramedImage matching appearance)` and createObject to attach it to the MainScreen page; only 11 of the 4×3=12 grid slots are occupied (11 categories verified), so place GAME in the **empty 12th tile** (geometry can be calculated from an adjacent delegate item or Constants)—visually it is the native 12th category. Tap → `System.doRun_debug_script(N)`.
- **C2 (real model row, optional advanced step)**: reassign `grid.model` (QML property bindings can be overridden imperatively): copy FavoriteModel rows into a ListModel + add a GAME row (roles: menuName/labelContext/label/iconSrc/itemEnabled). Tapping GAME emits the delegate's `loadSubMenuItem("GAME")` signal: the wrapper connects to that signal to handle launch first; the original ControlDrawer slot also fires, but `SubMenu.menuFile("GAME")` misses the map → returns an empty string (verified at 0x1706d9c) → loader reports "Ignore show loader without source", **harmless** (menuRw miss behavior needs hardware confirmation; worst case, one blocked toast).
- Advantages: ★☆ effort (entirely based on P2-verified injection techniques), no additional binary changes, hot iteration in seconds, no stock-tree changes, rollback=remove wrapper.
- Risks: MainScreen is loaded asynchronously by ControlDrawer's Loader (active triggered by swiping left)—injection must wait for the first left swipe (or prewarm with setMainState); touch is disabled in EVF mode (same native behavior, no issue); no dedicated GAME icon→use an existing image://svg/ic_general_* or emoji text.

### Comparison

| Dimension | A Full mirror | B Custom button | C Thin-wrapper overlay (recommended) |
|---|---|---|---|
| GAME visible in swipe-left menu | ✅ Native grid item | ❌ (physical button) | ✅ 12th-tile overlay (C2 can be a real model row) |
| Binary/patch changes | modqml.so +1 BSS location | None | None (reuse existing so) |
| QML effort | Mirror+edit MainScreen | 0 | ~30-80 lines of wrapper JS |
| Hot iteration | Edit /tmp files | Edit script | Edit /tmp files |
| New risks | Side effects of three appPrefix copies/icons | Read-only partition | Injection timing/empty-tile positioning |
| Execution backend | Same for all three: doRun_debug_script(N) → custom_debug_N.sh | | |

### Recommended route

1. **Implement C1** (1 hardware iteration): add a GAME tile to the thin wrapper + `System.doRun_debug_script(1)`; in /system/bin/custom_debug_1.sh (or another available number), exec /tmp/x2nes (mind the weston environment and weston cleanup sequence when restarting the GUI).
2. If the appearance is insufficient/joystick focus navigation is needed → upgrade to **C2** (model row) or **A** (full mirror, edit MainScreen.qml; assets ready).
3. Keep **B** as a wrapper-free fallback (button launches game).
4. Easter egg: add a `GuiConfig.showSecretMenuItems = true` switch in the GAME panel (native secret items appear immediately).

---

## 5. Assets added in this investigation

| File | Description |
|---|---|
| `core/qrc_dump.py` | Full rcc extractor (self-contained, with format-correction comments) |
| `core/qrc_dump/` | 72 trees, 1112 files + manifest.json (main tree t021600e8=346 QML; settings-menu files in qt/qml/app/qml/mainmenu/ + controlscreen/ControlDrawer.qml + viewmodels/StateMachineViewModel.qml) |
| `core/MENU_INJECT_NOTES.md` | This report |

## 6. Key address reference (fw v1.3.16.2, camera-gui offsets)

| Item | Offset |
|---|---|
| MenuX2d::topMenus / menuFile / menuLabel | 0x1706b40 / 0x1706ca8 (miss→empty string @0x1706d9c) / 0x1706b60 |
| Menu::conditionValid(E_MenuRW/E_MenuAvail) | 0x16f4ba8 / 0x16f4bb0 |
| Submenu::Submenu(QString,E_MenuOption,bool,E_MenuRW,E_MenuAvail) | 0x1703b08 |
| menux2d top-level Submenu construction sequence | 0x395724–0x396c34 (serviceMenu@0x396868 rw=NotImageProcessing(32); developerMenu@0x396c34 bool=1) |
| FavoriteModel::init/reloadModel/addItem | 0x16f1f80 / 0x16f1f48 / 0x16f2530 (singleton registration 0x16efb60) |
| SystemProxyDbus::doRun_debug_script(int) / run_debug_script(int,int,QObject*) | 0x1067dc0 / 0x1092ef8 |
| "Debug Script 1..4" value-table labels (UTF-16) | 0x1d16590 / 0x1d165ae / 0x1d165cc / 0x1d165ea |
| menuName+icon literal region for 11 categories (UTF-16) | 0x1cedb00–0x1cee566 (qrc:/qt/qml/app/qml/ prefix itself at 0x1cedb5c) |
| camera-system kScriptPathTemplate "/system/bin/custom_debug_%1.sh" | camera-system file offset 0x446290 (UTF-16) |
| kAppResourcePrefix three BSS copies | 0x2b054b8(app_main,patched in P1) / 0x2b05b10(svgimageprovider) / 0x2b494e8(CommonConstants, approach A target) |
