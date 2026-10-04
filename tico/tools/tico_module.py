#!/usr/bin/env python3
"""Settings definition and translations for the tico module of snes9x.

Run after merging upstream. It builds tools/dump_core_options.c against the
core's own libretro_core_options.h, so tico/module/settings.json lists exactly
the options the libnx core reads (snes9x_* keys, values and defaults), laid
out in tabs, followed by the overlay's own display and controls options.
Labels are translation keys; the strings go into tico/lang/*.json, taken from
tico's existing settings labels where an option already had one and otherwise
from the core's own translations. Choice labels stay English in settings.json;
the overlay translates them through settings_snes9x_value_* keys.

    python3 tico/tools/tico_module.py
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TICO = ROOT / "tico"
SETTINGS = TICO / "module/settings.json"
LANG_DIR = TICO / "lang"
LANGUAGES = ("en", "de", "es", "fr", "ja", "pt", "ru", "zh")
# tico-nx's own strings label the options it already knew; optional
TICO_NX = Path(os.environ.get("TICO_NX_DIR", ROOT.parents[1] / "tico-nx"))

# Core option -> label key. Options tico already labelled keep their key (and
# tico's translations); the rest are named after the option.
LABEL_KEYS = {
    "snes9x_region": "settings_snes9x_console_region",
    "snes9x_up_down_allowed": "settings_snes9x_opposing_directions",
    "snes9x_aspect": "settings_snes9x_aspect_ratio",
    "snes9x_overscan": "settings_snes9x_crop_overscan",
    "snes9x_hires_blend": "settings_snes9x_hi_res_blending",
    "snes9x_blargg": "settings_snes9x_blargg_ntsc_filter",
    "snes9x_gfx_transp": "settings_snes9x_transparency_effects",
    "snes9x_audio_interpolation": "settings_snes9x_interpolation",
    "snes9x_overclock_superfx": "settings_snes9x_superfx_overclock",
    "snes9x_overclock_cycles": "settings_snes9x_reduce_slowdown",
    "snes9x_reduce_sprite_flicker": "settings_snes9x_reduce_flickering",
    "snes9x_randomize_memory": "settings_snes9x_randomize_ram",
    "snes9x_block_invalid_vram_access": "settings_snes9x_block_invalid_vram",
    "snes9x_echo_buffer_hack": "settings_snes9x_echo_buffer_hack",
}

# Core options the frontend has no use for: light guns need pointer input it
# does not send, and the show_* options only hide entries in RetroArch's menu.
EXCLUDED = {
    "snes9x_show_lightgun_settings", "snes9x_lightgun_mode",
    "snes9x_superscope_reverse_buttons", "snes9x_superscope_crosshair",
    "snes9x_superscope_color", "snes9x_justifier1_crosshair", "snes9x_justifier1_color",
    "snes9x_justifier2_crosshair", "snes9x_justifier2_color", "snes9x_rifle_crosshair",
    "snes9x_rifle_color", "snes9x_show_advanced_av_settings",
}

# (tab, [(section, [option keys])]). Every core option the dump lists must be
# placed or excluded; the overlay's own tabs are added around these.
LAYOUT = [
    ("settings_snes9x_tab_system", [
        ("settings_snes9x_section_region", ["snes9x_region"]),
        ("settings_snes9x_section_input", ["snes9x_up_down_allowed"]),
    ]),
    ("settings_snes9x_tab_video", [
        ("settings_snes9x_section_display", ["snes9x_aspect", "snes9x_overscan"]),
        ("settings_snes9x_section_filters", ["snes9x_hires_blend", "snes9x_blargg"]),
        ("settings_snes9x_section_mode7", ["snes9x_mode7_hires", "snes9x_mode7_hires_bilinear"]),
        ("settings_snes9x_section_rendering", ["snes9x_gfx_transp"]),
    ]),
    ("settings_snes9x_tab_audio", [
        ("settings_snes9x_section_quality", ["snes9x_audio_interpolation",
                                             "snes9x_msu1_enhanced_audio"]),
    ]),
    ("settings_snes9x_tab_hacks", [
        ("settings_snes9x_section_performance", [
            "snes9x_overclock_superfx", "snes9x_superfx_timing", "snes9x_overclock_cycles",
        ]),
        ("settings_snes9x_section_compatibility", [
            "snes9x_reduce_sprite_flicker", "snes9x_randomize_memory",
            "snes9x_block_invalid_vram_access", "snes9x_echo_buffer_hack",
        ]),
    ]),
    ("settings_snes9x_tab_advanced", [
        ("settings_snes9x_section_layers", [
            "snes9x_layer_1", "snes9x_layer_2", "snes9x_layer_3", "snes9x_layer_4",
            "snes9x_layer_5",
        ]),
        ("settings_snes9x_section_sound_channels", [
            f"snes9x_sndchan_volume_{i}" for i in range(1, 9)
        ]),
    ]),
]

# Listed only while another option has a value, like the core's own menus.
DEPENDS_ON: dict[str, tuple[str, str]] = {}

# Switch buttons a SNES button (or the fast-forward hotkey) can sit on.
SWITCH_BUTTONS = [("A", "A"), ("B", "B"), ("X", "X"), ("Y", "Y"), ("L", "L"), ("R", "R"),
                  ("ZL", "ZL"), ("ZR", "ZR"), ("Plus", "Plus"), ("Minus", "Minus"),
                  ("StickL", "Left stick"), ("StickR", "Right stick"), ("Up", "Up"),
                  ("Down", "Down"), ("Left", "Left"), ("Right", "Right"), ("None", "Disabled")]

POSITIONS = [("hidden", "Hidden"), ("top_left", "Top left"), ("top_right", "Top right"),
             ("bottom_left", "Bottom left"), ("bottom_right", "Bottom right")]

# The overlay's own options: how the game is scaled, fast forward and the HUD.
# Shaders are picked in game (Settings > Shaders): tico cannot see the presets
# on the SD card.
OVERLAY_TAB = ("settings_snes9x_tab_display", [
    ("settings_snes9x_section_screen", [
        {"key": "display_mode", "label": "settings_snes9x_display_mode", "type": "enum",
         "default": "Display", "choices": [("Integer", "Integer"), ("Display", "Display")]},
        {"key": "display_size", "label": "settings_snes9x_display_size", "type": "enum",
         "default": "4:3", "choices": [("Stretch", "Stretch"), ("4:3", "4:3"), ("16:9", "16:9"),
                                             ("Original", "Original"), ("1x", "1x"), ("2x", "2x"),
                                             ("Auto", "Auto")]},
    ]),
    ("settings_snes9x_section_fast_forward", [
        {"key": "fast_forward_speed", "label": "settings_snes9x_fast_forward_speed", "type": "enum",
         "default": "200", "choices": [("150", "150%"), ("200", "200%"), ("300", "300%"),
                                        ("400", "400%"), ("unlimited", "Unlimited")]},
        {"key": "fast_forward_mode", "label": "settings_snes9x_fast_forward_mode", "type": "enum",
         "default": "hold", "choices": [("hold", "Hold"), ("toggle", "Toggle")]},
        {"key": "fast_forward_hotkey", "label": "settings_snes9x_fast_forward_hotkey", "type": "enum",
         "default": "ZR", "choices": SWITCH_BUTTONS},
    ]),
    ("settings_snes9x_section_hud", [
        {"key": "fps_counter_position", "label": "settings_snes9x_fps_counter", "type": "enum",
         "default": "hidden", "choices": POSITIONS},
        {"key": "rendered_ir_position", "label": "settings_snes9x_rendered_resolution",
         "type": "enum", "default": "hidden", "choices": POSITIONS},
    ]),
])

# The overlay's button mapping: SNES button -> Switch button. The SNES and
# Switch pads share a layout, so by default each sits on its namesake.
CONTROLS_TAB = ("settings_snes9x_tab_controls", [
    ("settings_snes9x_section_button_mapping", [
        {"key": key, "label": "settings_snes9x_" + key, "type": "enum", "default": default,
         "choices": SWITCH_BUTTONS}
        for key, default in [
            ("map_a", "A"), ("map_b", "B"), ("map_x", "X"), ("map_y", "Y"),
            ("map_l", "L"), ("map_r", "R"), ("map_start", "Plus"), ("map_select", "Minus"),
            ("map_up", "Up"), ("map_down", "Down"), ("map_left", "Left"), ("map_right", "Right"),
        ]
    ] + [
        {"key": "analog_dpad", "label": "settings_snes9x_analog_dpad", "type": "bool",
         "default": "enabled"},
    ]),
])

# Labels the core does not define. key -> (en, de, es, fr, ja, pt, ru, zh)
LABELS = {
    "settings_snes9x_tab_display": ("Display", "Anzeige", "Pantalla", "Affichage", "表示", "Tela",
                                    "Экран", "显示"),
    "settings_snes9x_tab_advanced": ("Advanced", "Erweitert", "Avanzado", "Avancé", "詳細",
                                     "Avançado", "Дополнительно", "高级"),
    "settings_snes9x_section_mode7": ("Mode 7", "Mode 7", "Modo 7", "Mode 7", "モード7", "Modo 7",
                                      "Режим 7", "模式7"),
    "settings_snes9x_section_layers": ("Layers", "Ebenen", "Capas", "Calques", "レイヤー",
                                       "Camadas", "Слои", "图层"),
    "settings_snes9x_section_sound_channels": ("Sound channels", "Tonkanäle", "Canales de sonido",
                                               "Canaux sonores", "サウンドチャンネル",
                                               "Canais de som", "Звуковые каналы", "声道"),
    "settings_snes9x_section_screen": ("Screen", "Bild", "Imagen", "Image", "画面", "Imagem",
                                       "Изображение", "画面"),
    "settings_snes9x_section_hud": ("On-screen info", "Bildschirmanzeige", "Información en pantalla",
                                    "Affichage à l'écran", "画面表示", "Informações na tela",
                                    "Экранная информация", "屏幕信息"),
    "settings_snes9x_display_mode": ("Display Mode", "Anzeigemodus", "Modo de pantalla",
                                     "Mode d'affichage", "表示モード", "Modo de exibição",
                                     "Режим отображения", "显示模式"),
    "settings_snes9x_display_size": ("Size", "Größe", "Tamaño", "Taille", "サイズ", "Tamanho",
                                     "Размер", "尺寸"),
    "settings_snes9x_fps_counter": ("FPS counter", "FPS-Zähler", "Contador de FPS",
                                    "Compteur de FPS", "FPSカウンター", "Contador de FPS",
                                    "Счётчик FPS", "帧率计数器"),
    "settings_snes9x_rendered_resolution": ("Rendered resolution", "Gerenderte Auflösung",
                                            "Resolución renderizada", "Résolution de rendu",
                                            "描画解像度", "Resolução renderizada",
                                            "Разрешение рендеринга", "渲染分辨率"),
    "settings_snes9x_section_fast_forward": ("Fast Forward", "Vorspulen", "Avance rápido",
                                             "Avance rapide", "早送り", "Avanço rápido",
                                             "Перемотка", "快进"),
    "settings_snes9x_fast_forward_speed": ("Fast forward speed", "Vorspul-Geschwindigkeit",
                                           "Velocidad de avance rápido", "Vitesse d'avance rapide",
                                           "早送りの速度", "Velocidade do avanço rápido",
                                           "Скорость перемотки", "快进速度"),
    "settings_snes9x_fast_forward_mode": ("Fast forward mode", "Vorspul-Modus",
                                          "Modo de avance rápido", "Mode d'avance rapide",
                                          "早送りモード", "Modo do avanço rápido",
                                          "Режим перемотки", "快进模式"),
    "settings_snes9x_fast_forward_hotkey": ("Fast forward button", "Vorspul-Taste",
                                            "Botón de avance rápido", "Bouton d'avance rapide",
                                            "早送りボタン", "Botão do avanço rápido",
                                            "Кнопка перемотки", "快进按键"),
    "settings_snes9x_tab_controls": ("Controls", "Steuerung", "Controles", "Commandes", "操作",
                                     "Controles", "Управление", "控制"),
    "settings_snes9x_section_button_mapping": ("Button mapping", "Tastenbelegung",
                                               "Asignación de botones", "Attribution des boutons",
                                               "ボタン割り当て", "Mapeamento de botões",
                                               "Назначение кнопок", "按键映射"),
    "settings_snes9x_map_a": ("A", "A", "A", "A", "A", "A", "A", "A"),
    "settings_snes9x_map_b": ("B", "B", "B", "B", "B", "B", "B", "B"),
    "settings_snes9x_map_x": ("X", "X", "X", "X", "X", "X", "X", "X"),
    "settings_snes9x_map_y": ("Y", "Y", "Y", "Y", "Y", "Y", "Y", "Y"),
    "settings_snes9x_map_l": ("L", "L", "L", "L", "L", "L", "L", "L"),
    "settings_snes9x_map_r": ("R", "R", "R", "R", "R", "R", "R", "R"),
    "settings_snes9x_map_start": ("Start", "Start", "Start", "Start", "スタート", "Start",
                                  "Start", "开始"),
    "settings_snes9x_map_select": ("Select", "Select", "Select", "Select", "セレクト", "Select",
                                   "Select", "选择"),
    "settings_snes9x_map_up": ("D-Pad Up", "Steuerkreuz oben", "Cruceta arriba",
                               "Croix haut", "十字キー上", "Direcional para cima",
                               "Крестовина вверх", "方向键上"),
    "settings_snes9x_map_down": ("D-Pad Down", "Steuerkreuz unten", "Cruceta abajo",
                                 "Croix bas", "十字キー下", "Direcional para baixo",
                                 "Крестовина вниз", "方向键下"),
    "settings_snes9x_map_left": ("D-Pad Left", "Steuerkreuz links", "Cruceta izquierda",
                                 "Croix gauche", "十字キー左", "Direcional para a esquerda",
                                 "Крестовина влево", "方向键左"),
    "settings_snes9x_map_right": ("D-Pad Right", "Steuerkreuz rechts", "Cruceta derecha",
                                  "Croix droite", "十字キー右", "Direcional para a direita",
                                  "Крестовина вправо", "方向键右"),
    "settings_snes9x_analog_dpad": ("Left stick as D-Pad", "Linker Stick als Steuerkreuz",
                                    "Stick izquierdo como cruceta",
                                    "Stick gauche comme croix directionnelle",
                                    "左スティックを十字キーとして使う",
                                    "Analógico esquerdo como direcional",
                                    "Левый стик как крестовина", "左摇杆作为方向键"),
    # options tico did not have before; the core only describes them in English
    "settings_snes9x_mode7_hires": ("HD Mode 7 scale", "HD-Mode-7-Skalierung", "Escala HD del modo 7",
                                    "Échelle HD du mode 7", "HDモード7の倍率", "Escala HD do Modo 7",
                                    "Масштаб HD Mode 7", "HD 模式7 缩放"),
    "settings_snes9x_mode7_hires_bilinear": ("HD Mode 7 filtering", "HD-Mode-7-Filterung",
                                             "Filtrado HD del modo 7", "Filtrage HD du mode 7",
                                             "HDモード7のフィルタリング", "Filtragem HD do Modo 7",
                                             "Фильтрация HD Mode 7", "HD 模式7 过滤"),
    "settings_snes9x_msu1_enhanced_audio": ("MSU-1 enhanced audio", "MSU-1-Audio", "Audio mejorado MSU-1",
                                            "Audio amélioré MSU-1", "MSU-1 拡張オーディオ",
                                            "Áudio aprimorado MSU-1", "Улучшенный звук MSU-1",
                                            "MSU-1 增强音频"),
    "settings_snes9x_superfx_timing": ("SuperFX timing (experimental)", "SuperFX-Timing (experimentell)",
                                       "Sincronización SuperFX (experimental)",
                                       "Synchronisation SuperFX (expérimental)",
                                       "SuperFX タイミング（実験的）", "Temporização SuperFX (experimental)",
                                       "Тайминг SuperFX (экспериментально)", "SuperFX 时序（实验性）"),
    "settings_snes9x_layer_1": ("Show layer 1", "Ebene 1 anzeigen", "Mostrar capa 1",
                                 "Afficher le calque 1", "レイヤー1を表示", "Mostrar camada 1",
                                 "Показывать слой 1", "显示图层1"),
    "settings_snes9x_layer_2": ("Show layer 2", "Ebene 2 anzeigen", "Mostrar capa 2",
                                 "Afficher le calque 2", "レイヤー2を表示", "Mostrar camada 2",
                                 "Показывать слой 2", "显示图层2"),
    "settings_snes9x_layer_3": ("Show layer 3", "Ebene 3 anzeigen", "Mostrar capa 3",
                                 "Afficher le calque 3", "レイヤー3を表示", "Mostrar camada 3",
                                 "Показывать слой 3", "显示图层3"),
    "settings_snes9x_layer_4": ("Show layer 4", "Ebene 4 anzeigen", "Mostrar capa 4",
                                 "Afficher le calque 4", "レイヤー4を表示", "Mostrar camada 4",
                                 "Показывать слой 4", "显示图层4"),
    "settings_snes9x_layer_5": ("Show sprites", "Sprites anzeigen", "Mostrar sprites",
                                "Afficher les sprites", "スプライトを表示", "Mostrar sprites",
                                "Показывать спрайты", "显示精灵"),
    "settings_snes9x_sndchan_volume_1": ("Channel 1 volume", "Lautstärke Kanal 1",
                                         "Volumen del canal 1", "Volume du canal 1",
                                         "チャンネル1の音量", "Volume do canal 1",
                                         "Громкость канала 1", "声道1音量"),
    "settings_snes9x_sndchan_volume_2": ("Channel 2 volume", "Lautstärke Kanal 2",
                                         "Volumen del canal 2", "Volume du canal 2",
                                         "チャンネル2の音量", "Volume do canal 2",
                                         "Громкость канала 2", "声道2音量"),
    "settings_snes9x_sndchan_volume_3": ("Channel 3 volume", "Lautstärke Kanal 3",
                                         "Volumen del canal 3", "Volume du canal 3",
                                         "チャンネル3の音量", "Volume do canal 3",
                                         "Громкость канала 3", "声道3音量"),
    "settings_snes9x_sndchan_volume_4": ("Channel 4 volume", "Lautstärke Kanal 4",
                                         "Volumen del canal 4", "Volume du canal 4",
                                         "チャンネル4の音量", "Volume do canal 4",
                                         "Громкость канала 4", "声道4音量"),
    "settings_snes9x_sndchan_volume_5": ("Channel 5 volume", "Lautstärke Kanal 5",
                                         "Volumen del canal 5", "Volume du canal 5",
                                         "チャンネル5の音量", "Volume do canal 5",
                                         "Громкость канала 5", "声道5音量"),
    "settings_snes9x_sndchan_volume_6": ("Channel 6 volume", "Lautstärke Kanal 6",
                                         "Volumen del canal 6", "Volume du canal 6",
                                         "チャンネル6の音量", "Volume do canal 6",
                                         "Громкость канала 6", "声道6音量"),
    "settings_snes9x_sndchan_volume_7": ("Channel 7 volume", "Lautstärke Kanal 7",
                                         "Volumen del canal 7", "Volume du canal 7",
                                         "チャンネル7の音量", "Volume do canal 7",
                                         "Громкость канала 7", "声道7音量"),
    "settings_snes9x_sndchan_volume_8": ("Channel 8 volume", "Lautstärke Kanal 8",
                                         "Volumen del canal 8", "Volume du canal 8",
                                         "チャンネル8の音量", "Volume do canal 8",
                                         "Громкость канала 8", "声道8音量"),
}

# Choice labels the core does not translate. English -> (de, es, fr, ja, pt, ru, zh)
CHOICES = {
    "Disabled": ("Deaktiviert", "Desactivado", "Désactivé", "無効", "Desativado", "Выключено",
                 "禁用"),
    "Integer": ("Ganzzahlig", "Entero", "Entier", "整数倍", "Inteiro", "Целочисленный", "整数"),
    "Display": ("Anzeige", "Pantalla", "Écran", "画面", "Tela", "Экран", "屏幕"),
    "Stretch": ("Strecken", "Estirar", "Étirer", "引き伸ばし", "Esticar", "Растянуть", "拉伸"),
    "Original": ("Original", "Original", "Original", "オリジナル", "Original", "Оригинал", "原始"),
    "Auto": ("Auto", "Auto", "Auto", "自動", "Auto", "Авто", "自动"),
    "Unlimited": ("Unbegrenzt", "Ilimitado", "Illimité", "無制限", "Ilimitado", "Без ограничений",
                  "无限制"),
    "Hold": ("Halten", "Mantener", "Maintenir", "長押し", "Segurar", "Удерживать", "按住"),
    "Toggle": ("Umschalten", "Alternar", "Basculer", "切り替え", "Alternar", "Переключать", "切换"),
    "Right stick": ("Rechter Stick", "Stick derecho", "Stick droit", "右スティック", "Analógico direito",
                    "Правый стик", "右摇杆"),
    "Left stick": ("Linker Stick", "Stick izquierdo", "Stick gauche", "左スティック", "Analógico esquerdo",
                   "Левый стик", "左摇杆"),
    "Plus": ("Plus", "Más", "Plus", "プラス", "Mais", "Плюс", "加号"),
    "Minus": ("Minus", "Menos", "Moins", "マイナス", "Menos", "Минус", "减号"),
    "Up": ("Oben", "Arriba", "Haut", "上", "Cima", "Вверх", "上"),
    "Down": ("Unten", "Abajo", "Bas", "下", "Baixo", "Вниз", "下"),
    "Left": ("Links", "Izquierda", "Gauche", "左", "Esquerda", "Влево", "左"),
    "Right": ("Rechts", "Derecha", "Droite", "右", "Direita", "Вправо", "右"),
    "Hidden": ("Ausgeblendet", "Oculto", "Masqué", "非表示", "Oculto", "Скрыто", "隐藏"),
    "Top left": ("Oben links", "Arriba a la izquierda", "En haut à gauche", "左上",
                 "Superior esquerdo", "Сверху слева", "左上"),
    "Top right": ("Oben rechts", "Arriba a la derecha", "En haut à droite", "右上",
                  "Superior direito", "Сверху справа", "右上"),
    "Bottom left": ("Unten links", "Abajo a la izquierda", "En bas à gauche", "左下",
                    "Inferior esquerdo", "Снизу слева", "左下"),
    "Bottom right": ("Unten rechts", "Abajo a la derecha", "En bas à droite", "右下",
                     "Inferior direito", "Снизу справа", "右下"),
}

RESTART_SUFFIX = re.compile(r"\s*\((Restart Required|Reload Core|[^)]*[Nn]eustart[^)]*|[^)]*[Rr]einici[^)]*|"
                            r"[^)]*[Rr]edémarr[^)]*|[^)]*再起動[^)]*|[^)]*перезапуск[^)]*|"
                            r"[^)]*重启[^)]*|[^)]*[Rr]einicializa[^)]*)\)")


def value_key(label: str) -> str:
    """tico_config.cpp's ValueKey: settings_snes9x_value_ + label as a slug."""
    return "settings_snes9x_value_" + "_".join(re.findall(r"[a-z0-9]+", label.lower()))


def clean_label(text: str) -> str:
    return RESTART_SUFFIX.sub("", text).lstrip("> ").strip()


def english_choice(value: str, label: str) -> str:
    # the core leaves on/off style values unlabelled
    return label.capitalize() if label == value and value in ("disabled", "enabled") else label


def dump_options() -> dict:
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "dump_core_options"
        subprocess.run(["cc", "-std=gnu11", "-w",
                        "-I", str(ROOT / "libretro"),
                        "-I", str(ROOT / "libretro/libretro-common/include"),
                        "-o", str(exe), str(Path(__file__).with_name("dump_core_options.c"))],
                       check=True)
        return json.loads(subprocess.run([str(exe)], check=True, capture_output=True,
                                         text=True).stdout)


def label_key(key: str) -> str:
    return LABEL_KEYS.get(key, "settings_snes9x_" + key.removeprefix("snes9x_"))


def build_settings(dump: dict) -> dict:
    core = {o["key"]: o for o in dump["en"]}
    placed = {key for _, sections in LAYOUT for _, keys in sections for key in keys}
    missing = sorted(set(core) - placed - EXCLUDED)
    if missing:
        raise SystemExit(f"core options not placed in LAYOUT: {missing}")

    tabs = []
    for tab, sections in LAYOUT:
        out_sections = []
        for title, keys in sections:
            options = []
            for key in keys:
                source = core[key]
                values = [v for v, _ in source["values"]]
                option = {"key": key, "label": label_key(key)}
                if sorted(values) == ["disabled", "enabled"]:
                    option.update(type="bool", default=source["default"])
                else:
                    option.update(type="enum", default=source["default"], choices=[
                        {"label": english_choice(v, l), "value": v} for v, l in source["values"]])
                if RESTART_SUFFIX.search(source["desc"]):
                    option["restart"] = True
                if key in DEPENDS_ON:
                    on, value = DEPENDS_ON[key]
                    option["depends_on"] = {"key": on, "value": value}
                options.append(option)
            out_sections.append({"title": title, "options": options})
        tabs.append({"name": tab, "sections": out_sections})

    def overlay_tab(definition):
        tab, sections = definition
        return {"name": tab, "sections": [
            {"title": title, "options": [
                {**o, "choices": [{"label": l, "value": v} for v, l in o["choices"]]}
                if "choices" in o else dict(o)
                for o in options]}
            for title, options in sections]}

    tabs.insert(1, overlay_tab(OVERLAY_TAB))
    tabs.append(overlay_tab(CONTROLS_TAB))

    return {
        "core_id": "snes9x",
        "display_name": "Snes9x",
        "config_file": "snes9x.jsonc",
        "slugs": ["snes"],
        "bool_true_value": "enabled",
        "bool_false_value": "disabled",
        "tabs": tabs,
    }


def tico_owned(key: str) -> bool:
    """Labels tico itself defines, whose wording tico keeps."""
    return key in LABEL_KEYS.values() or key.startswith(("settings_snes9x_tab_",
                                                          "settings_snes9x_section_"))


def build_strings(dump: dict, settings: dict,
                  existing: dict[str, dict[str, str]]) -> dict[str, dict[str, str]]:
    strings: dict[str, dict[str, str]] = {lang: {} for lang in LANGUAGES}
    english = {o["key"]: o for o in dump["en"]}
    for lang in LANGUAGES:
        out = strings[lang]
        index = LANGUAGES.index(lang)
        for option in dump[lang]:
            if option["key"] in EXCLUDED:
                continue
            key, en = option["key"], english[option["key"]]
            out[label_key(key)] = clean_label(option["desc"])
            for (value, label), (_, en_label) in zip(option["values"], en["values"]):
                en_text = english_choice(value, en_label)
                if label != en_label:
                    out[value_key(en_text)] = label
        for key, texts in LABELS.items():
            out[key] = texts[index]
        if lang != "en":
            for choice, translations in CHOICES.items():
                out[value_key(choice)] = translations[index - 1]
        if lang == "en":
            for key in list(out):
                if key.startswith("settings_snes9x_value_"):
                    del out[key]
    # tico's own labels keep tico's wording: from tico-nx when it is checked
    # out, otherwise as the language files already have them
    for lang in LANGUAGES:
        path = TICO_NX / "assets/lang" / f"{lang}.json"
        source = json.loads(path.read_text()) if path.exists() else existing[lang]
        for key, value in source.items():
            if tico_owned(key):
                strings[lang][key] = value
    used = set()

    def walk(node):
        if isinstance(node, dict):
            for field in ("label", "name", "title"):
                if isinstance(node.get(field), str) and node[field].startswith("settings_"):
                    used.add(node[field])
            for child in node.values():
                walk(child)
        elif isinstance(node, list):
            for child in node:
                walk(child)

    walk(settings)
    unlabelled = sorted(used - set(strings["en"]))
    if unlabelled:
        raise SystemExit(f"labels without English text: {unlabelled}")
    return strings


def main() -> None:
    dump = dump_options()
    settings = build_settings(dump)
    SETTINGS.write_text(json.dumps(settings, indent=2, ensure_ascii=False) + "\n")
    current_files = {lang: json.loads((LANG_DIR / f"{lang}.json").read_text())
                     if (LANG_DIR / f"{lang}.json").exists() else {} for lang in LANGUAGES}
    for lang, strings in build_strings(dump, settings, current_files).items():
        path = LANG_DIR / f"{lang}.json"
        current = {k: v for k, v in current_files[lang].items()
                   if not k.startswith("settings_snes9x_")}
        current.update(dict(sorted(strings.items())))
        path.write_text(json.dumps(current, indent=4, ensure_ascii=False) + "\n")
    print(f"wrote {SETTINGS.relative_to(ROOT)} and {len(LANGUAGES)} language files")


if __name__ == "__main__":
    main()
