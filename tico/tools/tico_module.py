#!/usr/bin/env python3
"""Settings definition and translations for the tico module of mupen64plus.

tico/module/settings.json lists every setting tico's settings screen and the
quick menu show: the core options the emulator reads (mupen64plus-* keys, in
tools/core_options.json with their values and defaults), laid out in tabs,
then the frontend's own (renderer, display, fast forward, HUD, controls).
Labels are translation keys; the strings go into tico/lang/*.json.

core_options.json was dumped from the core option table the libretro build
of this core carried; the native glue (tico/m64p/tico_m64p.c) reads the same
keys and values. Add an option there and here together.

    python3 tico/tools/tico_module.py
"""

from __future__ import annotations

import json
import re
from collections import OrderedDict
from pathlib import Path

TICO = Path(__file__).resolve().parents[1]
CORE_OPTIONS = TICO / "tools/core_options.json"
SETTINGS = TICO / "module/settings.json"
LANG_DIR = TICO / "lang"
# the overlay's shared strings, translated in tico-snes9x
SNES9X_LANG = TICO.parents[1] / "tico-snes9x/tico/lang"
LANGUAGES = ("en", "de", "es", "fr", "ja", "pt", "ru", "zh")
P = "settings_mupen64plus_"

# Options the frontend decides instead of the user: the renderer setting picks
# the RDP and RSP plugins, GLideN64 never uses its threaded wrapper here, and
# the Controls tab maps buttons.
EXCLUDED = {
    "mupen64plus-rdp-plugin", "mupen64plus-rsp-plugin", "mupen64plus-ThreadedRenderer",
    "mupen64plus-r-cbutton", "mupen64plus-l-cbutton", "mupen64plus-d-cbutton",
    "mupen64plus-u-cbutton", "mupen64plus-alt-map",
}

# Read while the game runs (tico_m64p_apply_options, or by the frontend);
# everything else applies when the game next starts.
LIVE = {
    "mupen64plus-parallel-rdp-synchronous", "mupen64plus-parallel-rdp-overscan",
    "mupen64plus-parallel-rdp-divot-filter", "mupen64plus-parallel-rdp-gamma-dither",
    "mupen64plus-parallel-rdp-vi-aa", "mupen64plus-parallel-rdp-vi-bilinear",
    "mupen64plus-parallel-rdp-dither-filter", "mupen64plus-parallel-rdp-downscaling",
    "mupen64plus-parallel-rdp-native-texture-lod", "mupen64plus-parallel-rdp-native-tex-rect",
    "mupen64plus-parallel-rdp-deinterlace-method", "mupen64plus-pak1", "mupen64plus-pak2",
    "mupen64plus-pak3", "mupen64plus-pak4", "mupen64plus-astick-deadzone",
    "mupen64plus-astick-sensitivity",
}

# tab -> sections -> option keys (core options by key, the rest built below)
LAYOUT = [
    ("tab_system", [
        ("section_renderer", ["tico_renderer"]),
        ("section_emulation", ["mupen64plus-cpucore", "mupen64plus-Framerate", "mupen64plus-virefresh",
                               "mupen64plus-CountPerOp", "mupen64plus-CountPerOpDenomPot",
                               "mupen64plus-FrameDuping"]),
        ("section_compatibility", ["mupen64plus-ForceDisableExtraMem", "mupen64plus-IgnoreTLBExceptions",
                                   "mupen64plus-parallel-rsp-hle-audio"]),
    ]),
    ("tab_display", [
        ("section_screen", ["display_mode", "display_size"]),
        ("section_fast_forward", ["fast_forward_speed", "fast_forward_mode", "fast_forward_hotkey"]),
        ("section_hud", ["fps_counter_position", "rendered_ir_position"]),
    ]),
    ("tab_gliden64", [
        ("section_resolution", ["mupen64plus-aspect", "mupen64plus-43screensize", "mupen64plus-169screensize",
                                "mupen64plus-EnableNativeResFactor"]),
        ("section_filtering", ["mupen64plus-BilinearMode", "mupen64plus-HybridFilter", "mupen64plus-MultiSampling",
                               "mupen64plus-FXAA", "mupen64plus-DitheringPattern",
                               "mupen64plus-DitheringQuantization", "mupen64plus-RDRAMImageDitheringMode"]),
        ("section_framebuffer", ["mupen64plus-EnableFBEmulation", "mupen64plus-EnableCopyColorToRDRAM",
                                 "mupen64plus-EnableCopyColorFromRDRAM", "mupen64plus-EnableCopyDepthToRDRAM",
                                 "mupen64plus-EnableCopyAuxToRDRAM", "mupen64plus-EnableN64DepthCompare",
                                 "mupen64plus-BackgroundMode"]),
        ("section_accuracy", ["mupen64plus-EnableLODEmulation", "mupen64plus-EnableHWLighting",
                              "mupen64plus-CorrectTexrectCoords", "mupen64plus-EnableNativeResTexrects",
                              "mupen64plus-EnableInaccurateTextureCoordinates", "mupen64plus-EnableTexCoordBounds",
                              "mupen64plus-EnableLegacyBlending", "mupen64plus-EnableFragmentDepthWrite",
                              "mupen64plus-EnableShadersStorage", "mupen64plus-GLideN64IniBehaviour"]),
        ("section_overscan", ["mupen64plus-EnableOverscan", "mupen64plus-OverscanTop", "mupen64plus-OverscanLeft",
                              "mupen64plus-OverscanRight", "mupen64plus-OverscanBottom"]),
        ("section_textures", ["mupen64plus-EnableTextureCache", "mupen64plus-MaxTxCacheSize",
                              "mupen64plus-txFilterMode", "mupen64plus-txEnhancementMode",
                              "mupen64plus-txFilterIgnoreBG", "mupen64plus-txCacheCompression",
                              "mupen64plus-EnableEnhancedTextureStorage", "mupen64plus-txHiresEnable",
                              "mupen64plus-txHiresFullAlphaChannel", "mupen64plus-EnableHiResAltCRC",
                              "mupen64plus-EnableEnhancedHighResStorage", "mupen64plus-MaxHiResTxVramLimit"]),
    ]),
    ("tab_parallel", [
        ("section_resolution", ["mupen64plus-parallel-rdp-upscaling", "mupen64plus-parallel-rdp-downscaling",
                                "mupen64plus-parallel-rdp-super-sampled-read-back",
                                "mupen64plus-parallel-rdp-super-sampled-read-back-dither",
                                "mupen64plus-parallel-rdp-overscan"]),
        ("section_vi", ["mupen64plus-parallel-rdp-vi-aa", "mupen64plus-parallel-rdp-vi-bilinear",
                        "mupen64plus-parallel-rdp-divot-filter", "mupen64plus-parallel-rdp-gamma-dither",
                        "mupen64plus-parallel-rdp-dither-filter", "mupen64plus-parallel-rdp-deinterlace-method"]),
        ("section_accuracy", ["mupen64plus-parallel-rdp-synchronous", "mupen64plus-parallel-rdp-native-texture-lod",
                              "mupen64plus-parallel-rdp-native-tex-rect"]),
    ]),
    ("tab_controls", [
        ("section_stick", ["mupen64plus-astick-deadzone", "mupen64plus-astick-sensitivity"]),
        ("section_paks", ["mupen64plus-pak1", "mupen64plus-pak2", "mupen64plus-pak3", "mupen64plus-pak4"]),
        ("section_button_mapping", ["map_a", "map_b", "map_z", "map_z_alt", "map_l", "map_r", "map_start",
                                    "map_c_up", "map_c_down", "map_c_left", "map_c_right",
                                    "map_up", "map_down", "map_left", "map_right"]),
    ]),
]

SWITCH_BUTTONS = [("A", "A"), ("B", "B"), ("X", "X"), ("Y", "Y"), ("L", "L"), ("R", "R"), ("ZL", "ZL"),
                  ("ZR", "ZR"), ("Plus", "Plus"), ("Minus", "Minus"), ("Left stick", "StickL"),
                  ("Right stick", "StickR"), ("Up", "Up"), ("Down", "Down"), ("Left", "Left"),
                  ("Right", "Right"), ("Disabled", "None")]
CORNERS = [("Hidden", "hidden"), ("Top left", "top_left"), ("Top right", "top_right"),
           ("Bottom left", "bottom_left"), ("Bottom right", "bottom_right")]


def enum(key, label, default, choices, **extra):
    option = {"key": key, "label": P + label, "type": "enum", "default": default,
              "choices": [{"label": l, "value": v} for l, v in choices]}
    option.update(extra)
    return option


# (key, label key, N64 button, default Switch button) as TicoMain's kButtonMappings
MAPPINGS = [("map_a", "map_a", "A"), ("map_b", "map_b", "B"), ("map_z", "map_z", "ZL"),
            ("map_z_alt", "map_z_alt", "ZR"), ("map_l", "map_l", "L"), ("map_r", "map_r", "R"),
            ("map_start", "map_start", "Plus"), ("map_c_up", "map_c_up", "None"),
            ("map_c_down", "map_c_down", "X"), ("map_c_left", "map_c_left", "Y"),
            ("map_c_right", "map_c_right", "None"), ("map_up", "map_up", "Up"),
            ("map_down", "map_down", "Down"), ("map_left", "map_left", "Left"),
            ("map_right", "map_right", "Right")]

FRONTEND = {
    # picked in tico only: the running game cannot switch renderers
    "tico_renderer": enum("tico_renderer", "renderer", "vk",
                          [("Vulkan (NVK)", "vk"), ("OpenGL (NVC0)", "gl"), ("Zink (OpenGL on NVK)", "zink")],
                          restart=True, tico_only=True),
    "display_mode": enum("display_mode", "display_mode", "Display", [("Integer", "Integer"), ("Display", "Display")]),
    "display_size": enum("display_size", "display_size", "4:3",
                         [("Stretch", "Stretch"), ("4:3", "4:3"), ("16:9", "16:9"), ("Original", "Original"),
                          ("1x", "1x"), ("2x", "2x"), ("Auto", "Auto")]),
    "fast_forward_speed": enum("fast_forward_speed", "fast_forward_speed", "200",
                               [("150%", "150"), ("200%", "200"), ("300%", "300"), ("400%", "400"),
                                ("Unlimited", "unlimited")]),
    "fast_forward_mode": enum("fast_forward_mode", "fast_forward_mode", "hold", [("Hold", "hold"), ("Toggle", "toggle")]),
    "fast_forward_hotkey": enum("fast_forward_hotkey", "fast_forward_hotkey", "None", SWITCH_BUTTONS),
    "fps_counter_position": enum("fps_counter_position", "fps_counter", "hidden", CORNERS),
    "rendered_ir_position": enum("rendered_ir_position", "rendered_resolution", "hidden", CORNERS),
}
for key, label, default in MAPPINGS:
    FRONTEND[key] = enum(key, label, default, SWITCH_BUTTONS)

# English strings of the frontend's own labels, and their translations.
STRINGS = {
    "tab_system": {"en": "System"},
    "tab_display": {"en": "Display"},
    "tab_controls": {"en": "Controls"},
    "tab_gliden64": {"en": "GLideN64"},
    "tab_parallel": {"en": "paraLLEl-RDP"},
    "section_renderer": {"en": "Renderer", "de": "Renderer", "es": "Renderizador", "fr": "Moteur de rendu",
                         "ja": "レンダラー", "pt": "Renderizador", "ru": "Рендерер", "zh": "渲染器"},
    "renderer": {"en": "Renderer", "de": "Renderer", "es": "Renderizador", "fr": "Moteur de rendu",
                 "ja": "レンダラー", "pt": "Renderizador", "ru": "Рендерер", "zh": "渲染器"},
    "section_emulation": {"en": "Emulation", "de": "Emulation", "es": "Emulación", "fr": "Émulation",
                          "ja": "エミュレーション", "pt": "Emulação", "ru": "Эмуляция", "zh": "模拟"},
    "section_compatibility": {"en": "Compatibility", "de": "Kompatibilität", "es": "Compatibilidad",
                              "fr": "Compatibilité", "ja": "互換性", "pt": "Compatibilidade",
                              "ru": "Совместимость", "zh": "兼容性"},
    "section_resolution": {"en": "Resolution", "de": "Auflösung", "es": "Resolución", "fr": "Résolution",
                           "ja": "解像度", "pt": "Resolução", "ru": "Разрешение", "zh": "分辨率"},
    "section_filtering": {"en": "Filtering", "de": "Filterung", "es": "Filtrado", "fr": "Filtrage",
                          "ja": "フィルタリング", "pt": "Filtragem", "ru": "Фильтрация", "zh": "过滤"},
    "section_framebuffer": {"en": "Frame Buffer", "de": "Framebuffer", "es": "Búfer de imagen",
                            "fr": "Tampon d'image", "ja": "フレームバッファ", "pt": "Buffer de quadros",
                            "ru": "Буфер кадра", "zh": "帧缓冲"},
    "section_accuracy": {"en": "Accuracy", "de": "Genauigkeit", "es": "Precisión", "fr": "Précision",
                         "ja": "精度", "pt": "Precisão", "ru": "Точность", "zh": "精确度"},
    "section_overscan": {"en": "Overscan", "de": "Overscan", "es": "Sobrebarrido", "fr": "Overscan",
                         "ja": "オーバースキャン", "pt": "Overscan", "ru": "Оверскан", "zh": "过扫描"},
    "section_textures": {"en": "Textures", "de": "Texturen", "es": "Texturas", "fr": "Textures",
                         "ja": "テクスチャ", "pt": "Texturas", "ru": "Текстуры", "zh": "纹理"},
    "section_vi": {"en": "Video Interface", "de": "Video-Schnittstelle", "es": "Interfaz de vídeo",
                   "fr": "Interface vidéo", "ja": "ビデオインターフェース", "pt": "Interface de vídeo",
                   "ru": "Видеоинтерфейс", "zh": "视频接口"},
    "section_stick": {"en": "Control Stick", "de": "Control-Stick", "es": "Stick de control",
                      "fr": "Stick de contrôle", "ja": "コントロールスティック", "pt": "Analógico",
                      "ru": "Стик", "zh": "摇杆"},
    "section_paks": {"en": "Controller Paks", "de": "Controller Paks", "es": "Accesorios del mando",
                     "fr": "Accessoires de manette", "ja": "コントローラーパック", "pt": "Acessórios do controle",
                     "ru": "Аксессуары контроллера", "zh": "手柄扩展卡"},
    "map_z": {"en": "Z", "de": "Z", "es": "Z", "fr": "Z", "ja": "Z", "pt": "Z", "ru": "Z", "zh": "Z"},
    "map_z_alt": {"en": "Z (second button)", "de": "Z (zweite Taste)", "es": "Z (segundo botón)",
                  "fr": "Z (deuxième bouton)", "ja": "Z (2つ目のボタン)", "pt": "Z (segundo botão)",
                  "ru": "Z (вторая кнопка)", "zh": "Z（第二按键）"},
    "map_c_up": {"en": "C-Up", "de": "C-Oben", "es": "C-Arriba", "fr": "C-Haut", "ja": "C上", "pt": "C-Cima",
                 "ru": "C-Вверх", "zh": "C上"},
    "map_c_down": {"en": "C-Down", "de": "C-Unten", "es": "C-Abajo", "fr": "C-Bas", "ja": "C下", "pt": "C-Baixo",
                   "ru": "C-Вниз", "zh": "C下"},
    "map_c_left": {"en": "C-Left", "de": "C-Links", "es": "C-Izquierda", "fr": "C-Gauche", "ja": "C左",
                   "pt": "C-Esquerda", "ru": "C-Влево", "zh": "C左"},
    "map_c_right": {"en": "C-Right", "de": "C-Rechts", "es": "C-Derecha", "fr": "C-Droite", "ja": "C右",
                    "pt": "C-Direita", "ru": "C-Вправо", "zh": "C右"},
}

# Labels shared with tico-snes9x, under its settings_snes9x_ names.
FROM_SNES9X = ["tab_system", "tab_display", "tab_controls", "section_screen", "section_fast_forward",
               "section_hud", "section_button_mapping", "display_mode", "display_size", "fast_forward_speed",
               "fast_forward_mode", "fast_forward_hotkey", "fps_counter", "rendered_resolution", "map_a", "map_b",
               "map_l", "map_r", "map_start", "map_up", "map_down", "map_left", "map_right"]


def slug(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_")


def core_option(entry) -> dict:
    key = entry["key"]
    values = [v for v, _ in entry["values"]]
    # a default the table leaves empty is its first value
    default = entry["default"] or values[0]
    option = {"key": key, "label": P + slug(key.replace("mupen64plus-", ""))}
    if sorted(values) == ["False", "True"]:
        option.update(type="bool", default=default)
    else:
        option.update(type="enum", default=default,
                      choices=[{"label": label, "value": value} for value, label in entry["values"]])
    if key not in LIVE:
        option["restart"] = True
    return option


def main() -> None:
    core = {e["key"]: e for e in json.loads(CORE_OPTIONS.read_text())}
    english = OrderedDict()

    tabs = []
    for tab, sections in LAYOUT:
        out_sections = []
        for title, keys in sections:
            options = []
            for key in keys:
                if key in core:
                    assert key not in EXCLUDED, key
                    options.append(core_option(core[key]))
                    english[options[-1]["label"]] = core[key]["desc"]
                else:
                    options.append(FRONTEND[key])
            out_sections.append({"title": P + title, "options": options})
        tabs.append({"name": P + tab, "sections": out_sections})

    listed = {o["key"] for t in tabs for s in t["sections"] for o in s["options"]}
    missing = set(core) - listed - EXCLUDED
    assert not missing, f"core options with no place in a tab: {sorted(missing)}"

    settings = OrderedDict([
        ("core_id", "mupen64plus"), ("display_name", "Mupen64Plus"), ("config_file", "mupen64plus.jsonc"),
        ("slugs", ["n64"]), ("bool_true_value", "True"), ("bool_false_value", "False"), ("tabs", tabs),
    ])
    SETTINGS.parent.mkdir(parents=True, exist_ok=True)
    SETTINGS.write_text(json.dumps(settings, indent=2, ensure_ascii=False) + "\n")

    for lang in LANGUAGES:
        snes9x = json.loads((SNES9X_LANG / f"{lang}.json").read_text())
        snes9x_en = json.loads((SNES9X_LANG / "en.json").read_text())
        strings = OrderedDict()
        # the overlay's own strings (menu, toasts, library, players)
        for key, value in snes9x.items():
            if not key.startswith("settings_snes9x_"):
                strings[key] = value
        for name in FROM_SNES9X:
            source = "settings_snes9x_" + name
            strings[P + name] = snes9x.get(source, snes9x_en[source])
        for name, texts in STRINGS.items():
            strings[P + name] = texts.get(lang, texts["en"])
        # the core's option names stay as the core spells them
        for key, text in english.items():
            strings.setdefault(key, text)
        (LANG_DIR / f"{lang}.json").write_text(json.dumps(strings, indent=4, ensure_ascii=False) + "\n")

    print(f"{SETTINGS.relative_to(TICO.parent)}: {len(listed)} options in {len(tabs)} tabs")


if __name__ == "__main__":
    main()
