#include "Input/InputTypes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// InputBindings - the text <-> bits boundary of the input system.
//
// The strings in settings.json are a stable, versionless contract: "kbm/space" means
// VK_SPACE in every build and "pad/a" means PadBtn::A whatever backend fills
// GamepadState. So the name tables are hand-written literals next to the parsers; never
// derive them from a Windows header, where a VK_* rename or an enum reorder would
// silently repoint a saved binding.
//
// Rules:
// 1. A parse failure never clears a binding: ParseBindingPath returns false and leaves
//    its outputs untouched, so a mangled settings.json cannot lock the player out of
//    moving.
// 2. Sanitize clamps scalars only; it never touches action[] or the move / look keys.
// The VK_* constants come from <windows.h> via the force-included pch.h.

namespace
{

// std::tolower is UB for negative values (high-bit UTF-8 bytes from settings.json): go
// through unsigned char first.
inline char Lower(char c) noexcept
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// Case-insensitive full-string compare. Bindings are authored by humans in a
// JSON file; "Space" and "space" must mean the same key.
bool IEquals(const char* a, const char* b) noexcept
{
    if (!a || !b) return false;
    while (*a && *b)
    {
        if (Lower(*a) != Lower(*b)) return false;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

// Compare exactly `aLen` bytes of `a` against the whole of the literal `lit`.
// Used for the device prefix, which is a slice of the path, not a C string.
bool IEqualsRange(const char* a, int aLen, const char* lit) noexcept
{
    for (int i = 0; i < aLen; ++i)
    {
        if (lit[i] == '\0') return false;
        if (Lower(a[i]) != Lower(lit[i])) return false;
    }
    return lit[aLen] == '\0';
}

const char* SkipSpace(const char* s) noexcept
{
    while (*s == ' ' || *s == '\t') ++s;
    return s;
}

// Clamp that also eats NaN / Inf. std::clamp returns a NaN unchanged, and one NaN
// stickDeadzone in a hand-edited file would make every deadzone test false and kill both
// sticks silently, so a non-finite value takes the compiled-in default.
void SanitizeScalar(float& v, float lo, float hi, float fallback) noexcept
{
    if (!std::isfinite(v)) { v = fallback; return; }
    v = std::clamp(v, lo, hi);
}

// ==== Name tables ====
// Linear scans on purpose: they run on settings load and rebind clicks, never per frame,
// so a hash map would only add a static ctor.

const char* const kActionNames[kGameActionCount] =
{
    "Jump", "Sprint", "Crouch", "Attack", "Dodge",
    "Interact", "MenuToggle", "UIConfirm", "UICancel", "Restart"
};

struct PadName { std::uint16_t mask; const char* name; };

// Canonical pad names, the only ones FormatBindingPath emits: lowercase-initial
// camelCase ("a", "leftShoulder").
const PadName kPadNames[] =
{
    { PadBtn::A,      "a"             },
    { PadBtn::B,      "b"             },
    { PadBtn::X,      "x"             },
    { PadBtn::Y,      "y"             },
    { PadBtn::LB,     "leftShoulder"  },
    { PadBtn::RB,     "rightShoulder" },
    { PadBtn::Back,   "back"          },
    { PadBtn::Start,  "start"         },
    { PadBtn::LS,     "leftStick"     },
    { PadBtn::RS,     "rightStick"    },
    { PadBtn::DUp,    "dpadUp"        },
    { PadBtn::DDown,  "dpadDown"      },
    { PadBtn::DLeft,  "dpadLeft"      },
    { PadBtn::DRight, "dpadRight"     },
    { PadBtn::LT,     "leftTrigger"   },
    { PadBtn::RT,     "rightTrigger"  },
};

// PARSE-ONLY aliases. Never emitted, so adding one can never change what an
// existing settings.json round-trips to.
const PadName kPadAliases[] =
{
    { PadBtn::LB,     "lb"           }, { PadBtn::LB,     "leftBumper"  },
    { PadBtn::LB,     "l1"           },
    { PadBtn::RB,     "rb"           }, { PadBtn::RB,     "rightBumper" },
    { PadBtn::RB,     "r1"           },
    { PadBtn::LS,     "ls"           }, { PadBtn::LS,     "l3"          },
    { PadBtn::LS,     "leftThumb"    },
    { PadBtn::RS,     "rs"           }, { PadBtn::RS,     "r3"          },
    { PadBtn::RS,     "rightThumb"   },
    { PadBtn::LT,     "lt"           }, { PadBtn::LT,     "l2"          },
    { PadBtn::RT,     "rt"           }, { PadBtn::RT,     "r2"          },
    { PadBtn::Back,   "select"       }, { PadBtn::Back,   "view"        },
    { PadBtn::Start,  "menu"         }, { PadBtn::Start,  "options"     },
    { PadBtn::DUp,    "up"           }, { PadBtn::DUp,    "dup"         },
    { PadBtn::DDown,  "down"         }, { PadBtn::DDown,  "ddown"       },
    { PadBtn::DLeft,  "left"         }, { PadBtn::DLeft,  "dleft"       },
    { PadBtn::DRight, "right"        }, { PadBtn::DRight, "dright"      },
};

// Single-character names for a-z / 0-9. VirtualKeyName hands out pointers into
// this array, so it MUST have static lifetime -- do not rebuild it into a local.
const char* const kAlnumNames[36] =
{
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
    "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"
};

struct VkName { std::uint16_t vk; const char* name; };

// CANONICAL key names. Lowercase, and FROZEN: these strings are inside every
// settings.json on every player's disk. Add rows freely; never rename one.
// a-z / 0-9 are handled procedurally against kAlnumNames and are absent here.
const VkName kVkNames[] =
{
    // Mouse buttons. VK_LBUTTON is a real virtual key, so the same ActionBinding
    // slot carries "click to attack" without a second code path.
    { VK_LBUTTON,  "mouse1"     }, { VK_RBUTTON,  "mouse2"     },
    { VK_MBUTTON,  "mouse3"     }, { VK_XBUTTON1, "mouse4"     },
    { VK_XBUTTON2, "mouse5"     },

    // Editing / whitespace
    { VK_SPACE,    "space"      }, { VK_RETURN,   "enter"      },
    { VK_ESCAPE,   "escape"     }, { VK_TAB,      "tab"        },
    { VK_BACK,     "backspace"  }, { VK_DELETE,   "delete"     },
    { VK_INSERT,   "insert"     },

    // Modifiers. Both the side-specific codes and the generic ones -- a player
    // who binds "shift" wants either shift, and that must survive a save.
    { VK_LSHIFT,   "lshift"     }, { VK_RSHIFT,   "rshift"     },
    { VK_SHIFT,    "shift"      },
    { VK_LCONTROL, "lctrl"      }, { VK_RCONTROL, "rctrl"      },
    { VK_CONTROL,  "ctrl"       },
    { VK_LMENU,    "lalt"       }, { VK_RMENU,    "ralt"       },
    { VK_MENU,     "alt"        },
    { VK_LWIN,     "lwin"       }, { VK_RWIN,     "rwin"       },
    { VK_CAPITAL,  "capslock"   },

    // Navigation
    { VK_UP,       "up"         }, { VK_DOWN,     "down"       },
    { VK_LEFT,     "left"       }, { VK_RIGHT,    "right"      },
    { VK_HOME,     "home"       }, { VK_END,      "end"        },
    { VK_PRIOR,    "pageup"     }, { VK_NEXT,     "pagedown"   },

    // Function row
    { VK_F1,  "f1"  }, { VK_F2,  "f2"  }, { VK_F3,  "f3"  }, { VK_F4,  "f4"  },
    { VK_F5,  "f5"  }, { VK_F6,  "f6"  }, { VK_F7,  "f7"  }, { VK_F8,  "f8"  },
    { VK_F9,  "f9"  }, { VK_F10, "f10" }, { VK_F11, "f11" }, { VK_F12, "f12" },

    // Numpad. Distinct from the digit row on purpose: "num1" and "1" are
    // different physical keys and a player may bind them differently.
    { VK_NUMPAD0, "num0" }, { VK_NUMPAD1, "num1" }, { VK_NUMPAD2, "num2" },
    { VK_NUMPAD3, "num3" }, { VK_NUMPAD4, "num4" }, { VK_NUMPAD5, "num5" },
    { VK_NUMPAD6, "num6" }, { VK_NUMPAD7, "num7" }, { VK_NUMPAD8, "num8" },
    { VK_NUMPAD9, "num9" },
    { VK_ADD,      "numadd"    }, { VK_SUBTRACT, "numsub"  },
    { VK_MULTIPLY, "nummul"    }, { VK_DIVIDE,   "numdiv"  },
    { VK_DECIMAL,  "numdec"    }, { VK_NUMLOCK,  "numlock" },

    // Punctuation. Named by WORD, not by glyph: the VK_OEM_* codes are
    // positional, so on a non-US layout the glyph moves but the code does not.
    // A word name at least stays unambiguous in the file.
    { VK_OEM_1,      "semicolon"  },   // ;:  on US
    { VK_OEM_PLUS,   "equals"     },   // =+
    { VK_OEM_COMMA,  "comma"      },   // ,<
    { VK_OEM_MINUS,  "minus"      },   // -_
    { VK_OEM_PERIOD, "period"     },   // .>
    { VK_OEM_2,      "slash"      },   // /?
    { VK_OEM_3,      "grave"      },   // `~
    { VK_OEM_4,      "lbracket"   },   // [{
    { VK_OEM_5,      "backslash"  },   // \|
    { VK_OEM_6,      "rbracket"   },   // ]}
    { VK_OEM_7,      "apostrophe" },   // '"

    // Misc
    { VK_PAUSE,    "pause"       },
    { VK_SNAPSHOT, "printscreen" },
    { VK_SCROLL,   "scrolllock"  },
};

// PARSE-ONLY key aliases, including the bare punctuation glyphs so a
// hand-edited file can say "," instead of "comma".
const VkName kVkAliases[] =
{
    { VK_RETURN,   "return"    },
    { VK_ESCAPE,   "esc"       },
    { VK_BACK,     "back"      },
    { VK_DELETE,   "del"       },
    { VK_PRIOR,    "pgup"      }, { VK_NEXT,     "pgdn"      },
    { VK_LCONTROL, "lcontrol"  }, { VK_RCONTROL, "rcontrol"  },
    { VK_CONTROL,  "control"   },
    { VK_LMENU,    "lmenu"     }, { VK_RMENU,    "rmenu"     },
    { VK_MENU,     "menu"      },
    { VK_LBUTTON,  "lmb"       }, { VK_RBUTTON,  "rmb"       },
    { VK_MBUTTON,  "mmb"       },
    { VK_OEM_1,      ";"       }, { VK_OEM_1,      "colon"   },
    { VK_OEM_PLUS,   "="       }, { VK_OEM_PLUS,   "plus"    },
    { VK_OEM_COMMA,  ","       },
    { VK_OEM_MINUS,  "-"       }, { VK_OEM_MINUS,  "dash"    },
    { VK_OEM_PERIOD, "."       }, { VK_OEM_PERIOD, "dot"     },
    { VK_OEM_2,      "/"       },
    { VK_OEM_3,      "`"       }, { VK_OEM_3,      "tilde"   },
    { VK_OEM_3,      "backquote" },
    { VK_OEM_4,      "["       },
    { VK_OEM_5,      "\\"      },
    { VK_OEM_6,      "]"       },
    { VK_OEM_7,      "'"       }, { VK_OEM_7,      "quote"   },
};

} // anonymous namespace

// ==== Enum <-> text ====

const char* ToString(InputDeviceKind d) noexcept
{
    switch (d)
    {
    case InputDeviceKind::None:          return "None";
    case InputDeviceKind::KeyboardMouse: return "KeyboardMouse";
    case InputDeviceKind::Gamepad:       return "Gamepad";
    default:                             return "Unknown";
    }
}

const char* ToString(GameAction a) noexcept
{
    const int i = static_cast<int>(a);
    if (i < 0 || i >= kGameActionCount) return "Unknown";
    return kActionNames[i];
}

bool ParseGameAction(const char* name, GameAction& out) noexcept
{
    if (!name) return false;
    name = SkipSpace(name);

    for (int i = 0; i < kGameActionCount; ++i)
    {
        if (IEquals(name, kActionNames[i]))
        {
            out = static_cast<GameAction>(i);
            return true;
        }
    }
    // Unknown action names are NOT an error the caller should paper over by
    // guessing: `out` stays untouched so a corrupt row is simply skipped.
    return false;
}

// ==== Pad buttons ====

const char* PadButtonName(std::uint16_t mask) noexcept
{
    // Exactly one bit, per the header contract. A multi-bit "any of these" mask
    // has no single name; the caller must split it (FormatBindingPath does).
    if (mask == 0 || (mask & static_cast<std::uint16_t>(mask - 1)) != 0)
        return nullptr;

    for (const PadName& p : kPadNames)
        if (p.mask == mask) return p.name;
    return nullptr;
}

std::uint16_t ParsePadButton(const char* name) noexcept
{
    if (!name) return PadBtn::None;
    name = SkipSpace(name);
    if (*name == '\0') return PadBtn::None;

    for (const PadName& p : kPadNames)
        if (IEquals(name, p.name)) return p.mask;
    for (const PadName& p : kPadAliases)
        if (IEquals(name, p.name)) return p.mask;
    return PadBtn::None;
}

// ==== Virtual keys ====

const char* VirtualKeyName(std::uint16_t vk) noexcept
{
    if (vk == 0) return nullptr;              // 0 == unbound, never a name

    // Letters and digits are their own name. Note there is no VK_* collision in
    // either range: 0x41-0x5A and 0x30-0x39 are exactly the ASCII codes.
    if (vk >= 'A' && vk <= 'Z') return kAlnumNames[vk - 'A'];
    if (vk >= '0' && vk <= '9') return kAlnumNames[26 + (vk - '0')];

    for (const VkName& k : kVkNames)
        if (k.vk == vk) return k.name;
    return nullptr;
}

std::uint16_t ParseVirtualKey(const char* name) noexcept
{
    if (!name) return 0;
    name = SkipSpace(name);
    if (name[0] == '\0') return 0;

    // A bare single character: "w" -> 'W', "5" -> '5'. This runs BEFORE the
    // numeric escape below, otherwise "5" would parse as raw VK 5 (VK_XBUTTON1)
    // instead of the digit key the author obviously meant.
    if (name[1] == '\0')
    {
        const unsigned char c = static_cast<unsigned char>(name[0]);
        if (std::isalpha(c))
            return static_cast<std::uint16_t>(std::toupper(c));
        if (std::isdigit(c))
            return static_cast<std::uint16_t>(c);
        // fall through: single-glyph punctuation lives in kVkAliases
    }

    for (const VkName& k : kVkNames)
        if (IEquals(name, k.name)) return k.vk;
    for (const VkName& k : kVkAliases)
        if (IEquals(name, k.name)) return k.vk;

    // Numeric escape -- "0x1B" or "27". Keeps hand-edited files and raw VK ints
    // working, and gives FormatBindingPath something to emit for an OEM key we
    // have no word for, so unnamed bindings still round-trip.
    const bool hex = (name[0] == '0' && (name[1] == 'x' || name[1] == 'X'));
    const char* digits = hex ? name + 2 : name;
    char* end = nullptr;
    const unsigned long v = std::strtoul(digits, &end, hex ? 16 : 10);
    if (end && end != digits && *end == '\0' && v >= 1 && v <= 255)
        return static_cast<std::uint16_t>(v);

    return 0;
}

// ==== Binding paths ====

bool ParseBindingPath(const char* path, bool& outIsPad,
                      std::uint16_t& outVk, std::uint16_t& outPad) noexcept
{
    if (!path) return false;
    path = SkipSpace(path);
    if (*path == '\0') return false;

    if (const char* slash = std::strchr(path, '/'))
    {
        const int   preLen = static_cast<int>(slash - path);
        const char* body   = SkipSpace(slash + 1);

        if (IEqualsRange(path, preLen, "pad"))
        {
            const std::uint16_t mask = ParsePadButton(body);
            if (mask == PadBtn::None) return false;
            outIsPad = true;
            outPad   = mask;
            outVk    = 0;        // the unused carrier, so a stale vk cannot leak
            return true;
        }
        if (IEqualsRange(path, preLen, "kbm") ||
            IEqualsRange(path, preLen, "kb")  ||
            IEqualsRange(path, preLen, "key"))
        {
            const std::uint16_t vk = ParseVirtualKey(body);
            if (vk == 0) return false;
            outIsPad = false;
            outVk    = vk;
            outPad   = 0;
            return true;
        }

        // Unknown prefix: outputs untouched, false returned, so the caller keeps its binding.
        // This is the lockout guard: a typo in settings.json costs one customisation, never the
        // ability to move.
        return false;
    }

    // No prefix: a keyboard key. An InputBindingOverride row carries its device in a separate
    // "device" field, so a hand-edited row may hold a bare path like "space" / "w".
    const std::uint16_t vk = ParseVirtualKey(path);
    if (vk == 0) return false;
    outIsPad = false;
    outVk    = vk;
    outPad   = 0;
    return true;
}

bool FormatBindingPath(bool isPad, std::uint16_t vkOrPad,
                       char* out, int cap) noexcept
{
    if (!out || cap <= 0) return false;
    out[0] = '\0';                     // never hand back an uninitialised buffer
    if (vkOrPad == 0) return false;    // unbound

    int n = -1;
    if (isPad)
    {
        // ActionBinding::pad is an OR of masks (any of them fires it), so a hand-edited file can
        // carry several bits. A multi-bit mask is bound, and this function fails only when
        // unbound or the buffer is short, so it names the lowest set bit; a caller that persists
        // the result keeps only that bit (EngineSettings warns when it does).
        const std::uint32_t m      = vkOrPad;
        const std::uint16_t lowest = static_cast<std::uint16_t>(m & (~m + 1u));

        const char* name = PadButtonName(lowest);
        if (!name) return false;
        n = std::snprintf(out, static_cast<size_t>(cap), "pad/%s", name);
    }
    else
    {
        // A virtual key is a byte, and ParseVirtualKey accepts only 1..255. Fail closed above
        // that rather than emit a token that parses back as another key; the caller keeps its
        // previous binding, as for an unknown path.
        if (vkOrPad > 255) return false;

        char        hex[16];
        const char* name = VirtualKeyName(vkOrPad);
        if (!name)
        {
            // An OEM key with no word name. Emit the numeric form that
            // ParseVirtualKey accepts so the binding survives a save/load cycle
            // instead of silently vanishing.
            std::snprintf(hex, sizeof(hex), "0x%02X", static_cast<unsigned>(vkOrPad));
            name = hex;
        }
        n = std::snprintf(out, static_cast<size_t>(cap), "kbm/%s", name);
    }

    // snprintf returns the length it WANTED; >= cap means it truncated.
    if (n < 0 || n >= cap) { out[0] = '\0'; return false; }
    return true;
}

// ==== Defaults / Sanitize ====

InputBindings InputBindings::Defaults() noexcept
{
    // Value-init so the tuning scalars come from the in-class initialisers in InputTypes.h,
    // their single source of truth; do not restate them here.
    //   stickDeadzone    0.24 ~= XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE 7849/32767 (one field for
    //                    both sticks; the right stick's 8689/32767 = 0.265 is tuning noise)
    //   triggerThreshold 0.12 ~= XINPUT_GAMEPAD_TRIGGER_THRESHOLD 30/255
    InputBindings b {};

    // Movement: D/A strafe, W/S forward, left stick.
    b.move.vkPosX = 'D';  b.move.vkNegX = 'A';
    b.move.vkPosY = 'W';  b.move.vkNegY = 'S';
    b.move.stick  = 0;                     // left stick

    // Look has no keyboard composite on purpose: the mouse carries look as a pixel
    // displacement (ActionState::lookMouseDX/DY), and a key composite would feed a rate into
    // that carrier and make look framerate-dependent (see ActionState in InputTypes.h).
    b.look.vkPosX = 0;  b.look.vkNegX = 0;
    b.look.vkPosY = 0;  b.look.vkNegY = 0;
    b.look.stick  = 1;                     // right stick

    auto bind = [&b](GameAction a, std::uint16_t vk, std::uint16_t pad) noexcept
    {
        ActionBinding& r = b.action[static_cast<int>(a)];
        r.vk[0] = vk;
        r.vk[1] = 0;
        r.pad   = pad;
    };

    bind(GameAction::Jump,       VK_SPACE,   PadBtn::A);
    // Sprint = L3 (left stick click), the console convention: the thumb is already on the
    // steering stick. Not a stick-magnitude threshold: magnitude never picks the gait (see
    // PlayerControllerSystem's speed block).
    bind(GameAction::Sprint,     VK_LSHIFT,  PadBtn::LS);
    bind(GameAction::Attack,     VK_LBUTTON, PadBtn::RT);
    bind(GameAction::MenuToggle, VK_ESCAPE,  PadBtn::Start);
    bind(GameAction::UIConfirm,  VK_RETURN,  PadBtn::A);
    bind(GameAction::UICancel,   VK_ESCAPE,  PadBtn::B);
    bind(GameAction::Dodge,      VK_CONTROL, PadBtn::B);
    bind(GameAction::Restart,    'R',        PadBtn::DUp);

    // Crouch and Interact ship unbound. Do not bind them until a consumer exists: nothing
    // reads CharacterControllerComponent::wantsCrouch and no engine system consumes
    // GameAction::Interact, so a binding would be input that works and does nothing, the
    // hardest bug to chase. Same rule at the GameAction enum in InputTypes.h.
    b.action[static_cast<int>(GameAction::Crouch)]   = ActionBinding{};
    b.action[static_cast<int>(GameAction::Interact)] = ActionBinding{};

    return b;
}

void InputBindings::Sanitize() noexcept
{
    // The ranges are the ones documented in InputTypes.h (the contract); keep them in step.
    // Fall back to Defaults(), not a value-initialised instance: look.stick's in-class
    // initialiser is 0 (left) while Defaults() sets 1 (right), so repairing look.stick to 0
    // would put look on the move stick and walking would spin the camera.
    const InputBindings d = InputBindings::Defaults();

    SanitizeScalar(lookRateDegPerSec, 10.f,  720.f, d.lookRateDegPerSec);
    SanitizeScalar(mouseLookScale,    0.05f,  20.f, d.mouseLookScale);
    SanitizeScalar(stickDeadzone,     0.f,    0.9f, d.stickDeadzone);
    SanitizeScalar(stickCurveExp,     1.f,    4.f,  d.stickCurveExp);
    SanitizeScalar(triggerThreshold,  0.f,    0.9f, d.triggerThreshold);

    // stick selects left (0) or right (1). Consumers pick with a ternary, so an out-of-range
    // value reads nothing out of bounds but silently means left: repair it to the shipped
    // default.
    if (move.stick > 1) move.stick = d.move.stick;
    if (look.stick > 1) look.stick = d.look.stick;

    // Deliberately untouched: action[] and the move / look vk fields. Every uint16 VK either
    // names a key or is never down, so validating could only zero a binding, the lockout
    // this path exists to prevent. Unknown paths are rejected upstream by ParseBindingPath,
    // which keeps the previous binding.
}
