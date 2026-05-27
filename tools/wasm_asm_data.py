#!/usr/bin/env python3
import re
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

ROOT = Path(__file__).resolve().parents[1]
LABEL_RE = re.compile(r"^([A-Za-z_.$][\w.$]*)(::?)\s*(.*)$")
ASSIGN_RE = re.compile(r"^([A-Za-z_.$][\w.$]*)\s*=\s*(.+)$")
INCLUDE_RE = re.compile(r'^\s*\.include\s+"([^"]+)"')
CPP_INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')
DEFINE_RE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)(?:\s+(.+))?$")


class AsmMacro:
    def __init__(self, params: List[Tuple[str, Optional[str]]], body: List[str]):
        self.params = params
        self.body = body


VARARG_DEFAULT = "__WASM_ASM_VARARG__"
FUNCTION_SYMBOLS: Optional[Set[str]] = None


def load_script_command_constants() -> Dict[str, int]:
    constants = {}
    value = 0
    table = ROOT / "data/script_cmd_table.inc"
    for line in table.read_text().splitlines():
        line = line.split("@", 1)[0].strip()
        if line.startswith("script_cmd_table_entry "):
            constants[line.split()[1]] = value
            value += 1
    return constants


def load_script_command_functions() -> List[str]:
    functions = []
    for line in (ROOT / "data/script_cmd_table.inc").read_text().splitlines():
        line = line.split("@", 1)[0].strip()
        if not line.startswith("script_cmd_table_entry "):
            continue
        functions.append(line.split()[2])
    return functions


def load_function_symbols() -> Set[str]:
    global FUNCTION_SYMBOLS
    if FUNCTION_SYMBOLS is not None:
        return FUNCTION_SYMBOLS

    symbols: Set[str] = set()
    pattern = re.compile(
        r"^[A-Za-z_][\w\s\*]*?\s+([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:\{|;)",
        re.MULTILINE,
    )
    for root_name in ("src", "include"):
        for path in (ROOT / root_name).glob("**/*.[ch]"):
            text = strip_c_comments(path.read_text(errors="ignore"))
            for match in pattern.finditer(text):
                name = match.group(1)
                if name not in {"if", "for", "while", "switch", "return"}:
                    symbols.add(name)
    FUNCTION_SYMBOLS = symbols
    return symbols


def load_special_constants() -> Dict[str, int]:
    constants = {}
    value = 0
    return_values = load_special_return_values()
    for line in (ROOT / "data/specials.inc").read_text().splitlines():
        line = strip_at_comment(line).strip()
        if not line.startswith("def_special "):
            continue
        args = split_args(line[len("def_special "):])
        name = args[0]
        constants[f"SPECIAL_{name}"] = value
        constants[f"SPECIAL_WAITSTATE_{name}"] = int("waitstate=1" in args)
        constants[f"SPECIAL_RETURNS_VALUE_{name}"] = int(return_values.get(name, True))
        value += 1
    return constants


def load_special_return_values() -> Dict[str, bool]:
    names = []
    for line in (ROOT / "data/specials.inc").read_text().splitlines():
        line = strip_at_comment(line).strip()
        if line.startswith("def_special "):
            names.append(split_args(line[len("def_special "):])[0])

    wanted = set(names)
    pattern = re.compile(r"^([A-Za-z_][\w\s\*]*?)\s+([A-Za-z_]\w*)\s*\([^;{]*\)\s*(?:\{|;)", re.MULTILINE)
    returns: Dict[str, bool] = {}
    for path in list((ROOT / "src").glob("*.c")) + list((ROOT / "include").glob("*.h")):
        for match in pattern.finditer(path.read_text(errors="ignore")):
            return_type, name = match.groups()
            if name not in wanted or name in returns:
                continue
            tokens = return_type.replace("*", " ").split()
            tokens = [token for token in tokens if token not in {"static", "UNUSED", "const"}]
            returns[name] = tokens != ["void"]
    return returns


def load_movement_constants() -> Dict[str, int]:
    constants = {}
    define_re = re.compile(r"#define\s+(MOVEMENT_ACTION_[A-Z0-9_]+)\s+(.+)$")
    for line in (ROOT / "include/constants/event_object_movement.h").read_text().splitlines():
        line = line.split("//", 1)[0].strip()
        match = define_re.match(line)
        if not match:
            continue
        name, expr = match.groups()
        constants[name] = parse_int(expr, constants)
    for line in (ROOT / "asm/macros/movement.inc").read_text().splitlines():
        line = strip_at_comment(line).strip()
        if not line.startswith("create_movement_action "):
            continue
        name, value = split_args(line[len("create_movement_action "):])
        constants[name] = parse_int(value, constants)
    return constants


def load_map_constants() -> Dict[str, int]:
    constants = {}
    define_re = re.compile(r"#define\s+(WARP_ID_[A-Z0-9_]+)\s+(.+)$")
    enum_value = 0
    in_enum = False
    for line in (ROOT / "include/constants/maps.h").read_text().splitlines():
        line = line.split("//", 1)[0].strip()
        if not line:
            continue
        if line.startswith("enum"):
            in_enum = True
            enum_value = 0
            continue
        if in_enum:
            if line.startswith("};"):
                in_enum = False
                continue
            entry = line.rstrip(",")
            if not entry or entry == "{":
                continue
            name, sep, expr = entry.partition("=")
            name = name.strip()
            if not name:
                continue
            value = parse_int(expr, constants) if sep else enum_value
            constants[name] = value
            enum_value = value + 1
            continue
        match = define_re.match(line)
        if match:
            name, expr = match.groups()
            constants[name] = parse_int(expr, constants)
    return constants


def strip_c_comments(text: str) -> str:
    return re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)


def normalize_c_int_expr(expr: str) -> str:
    expr = expr.split("//", 1)[0].strip()
    expr = re.sub(r"\b(0x[0-9A-Fa-f]+|\d+)[uUlL]+\b", r"\1", expr)
    expr = re.sub(r"\([^()]*\b(?:u8|s8|u16|s16|u32|s32|bool|bool8|bool32)\s*\)", "", expr)
    return expr


def load_source_constants(source: Optional[Path]) -> Dict[str, int]:
    if source is None:
        return {}

    constants: Dict[str, int] = {}
    seen = set()

    def resolve_include(include: str) -> Path:
        root_path = ROOT / include
        if root_path.exists():
            return root_path
        return ROOT / "include" / include

    def read_header(path: Path) -> None:
        path = path if path.is_absolute() else ROOT / path
        path = path.resolve()
        if path in seen or not path.exists():
            return
        seen.add(path)

        text = strip_c_comments(path.read_text(errors="ignore"))
        enum_value = 0
        in_enum = False
        for raw in text.splitlines():
            line = raw.strip()
            include_match = CPP_INCLUDE_RE.match(line)
            if include_match:
                read_header(resolve_include(include_match.group(1)))
                continue

            if in_enum:
                if "}" in line:
                    in_enum = False
                    continue
                entry = line.split("//", 1)[0].rstrip(",").strip()
                if not entry:
                    continue
                name, sep, expr = entry.partition("=")
                name = name.strip()
                if not re.fullmatch(r"[A-Za-z_]\w*", name):
                    continue
                value = eval_asm_expr(normalize_c_int_expr(expr), constants) if sep else enum_value
                if value is None:
                    continue
                constants[name] = value
                enum_value = value + 1
                continue

            if line.startswith("enum"):
                if "{" in line and "}" in line:
                    body = line.split("{", 1)[1].rsplit("}", 1)[0]
                    enum_value = 0
                    for entry in split_args(body):
                        name, sep, expr = entry.partition("=")
                        name = name.strip()
                        if not re.fullmatch(r"[A-Za-z_]\w*", name):
                            continue
                        value = eval_asm_expr(normalize_c_int_expr(expr), constants) if sep else enum_value
                        if value is None:
                            continue
                        constants[name] = value
                        enum_value = value + 1
                    continue
                if "{" in line:
                    enum_value = 0
                    in_enum = True
                continue

            match = DEFINE_RE.match(line)
            if not match:
                continue
            name, expr = match.groups()
            if "(" in name or expr is None:
                continue
            value = eval_asm_expr(normalize_c_int_expr(expr), constants)
            if value is not None:
                constants[name] = value

    for raw in source.read_text(errors="ignore").splitlines():
        match = CPP_INCLUDE_RE.match(raw)
        if match:
            read_header(resolve_include(match.group(1)))
    return constants


def preprocess(source: Path) -> str:
    first = subprocess.run(
        [str(ROOT / "tools/preproc/preproc"), str(source), "charmap.txt"],
        cwd=ROOT,
        check=True,
        text=True,
        capture_output=True,
    )
    cpp = subprocess.run(
        ["clang", "-E", "-x", "assembler-with-cpp", "-I", "include", "-"],
        cwd=ROOT,
        input=first.stdout,
        check=True,
        text=True,
        capture_output=True,
    )
    second = subprocess.run(
        [str(ROOT / "tools/preproc/preproc"), "-ie", str(source), "charmap.txt"],
        cwd=ROOT,
        input=cpp.stdout,
        check=True,
        text=True,
        capture_output=True,
    )
    return second.stdout


def parse_int(expr: str, constants: Dict[str, int]) -> int:
    return int(eval(expr.strip(), {"__builtins__": {}}, constants))


def split_args(text: str) -> List[str]:
    if not text:
        return []
    args = []
    current = []
    depth = 0
    for char in text:
        if char == "," and depth == 0:
            args.append("".join(current).strip())
            current = []
            continue
        if char == "(":
            depth += 1
        elif char == ")" and depth:
            depth -= 1
        current.append(char)
    args.append("".join(current).strip())
    while args and args[-1] == "":
        args.pop()
    return args


def parse_macro_args(name: str, text: str, macros: Dict[str, "AsmMacro"]) -> List[str]:
    if "," in text or name not in macros:
        return split_args(text)
    if len(macros[name].params) <= 1:
        return [text.strip()] if text.strip() else []
    if re.search(r"\s[+\-*/]\s|[()]", text):
        return [text.strip()]
    return text.split()


def collect_macro_includes(source: Path) -> List[Path]:
    paths: List[Path] = []
    seen = set()

    def visit(path: Path) -> None:
        path = path if path.is_absolute() else ROOT / path
        path = path.resolve()
        if path in seen or not path.exists():
            return
        seen.add(path)

        for raw in path.read_text().splitlines():
            match = INCLUDE_RE.match(strip_at_comment(raw))
            if not match:
                continue
            include = match.group(1)
            include_path = (ROOT / include).resolve()
            if include == "asm/macros.inc" or include_path.is_relative_to(ROOT / "asm/macros"):
                visit(include_path)

        if path == (ROOT / "asm/macros.inc").resolve() or path.is_relative_to(ROOT / "asm/macros"):
            paths.append(path)

    visit(source)
    return paths


def load_event_macros(source: Optional[Path] = None) -> Dict[str, AsmMacro]:
    macros: Dict[str, AsmMacro] = {}
    paths = collect_macro_includes(source) if source is not None else []
    if not paths:
        paths = sorted((ROOT / "asm/macros").glob("**/*.inc"))
    for path in paths:
        current_name = None
        current_params: List[Tuple[str, Optional[str]]] = []
        current_body: List[str] = []
        for raw in path.read_text().splitlines():
            stripped = strip_at_comment(raw).strip()
            if not stripped:
                continue
            if current_name is None:
                if not stripped.startswith(".macro "):
                    continue
                signature = stripped[len(".macro "):]
                name, _, params_text = signature.partition(" ")
                params = []
                for param in split_args(params_text):
                    if not param:
                        continue
                    if param.endswith(":vararg"):
                        params.append((param[:-7].strip(), VARARG_DEFAULT))
                        continue
                    if param.endswith(":req"):
                        param = param[:-4]
                    if "=" in param:
                        param_name, default = param.split("=", 1)
                        params.append((param_name.strip(), default.strip()))
                    else:
                        params.append((param.strip(), None))
                current_name = name
                current_params = params
                current_body = []
                continue

            if stripped == ".endm":
                macros[current_name] = AsmMacro(current_params, current_body)
                current_name = None
                continue
            current_body.append(stripped)

    return macros


def substitute_constants(line: str, constants: Dict[str, int]) -> str:
    return re.sub(
        r"\b[A-Z][A-Z0-9_]*\b",
        lambda match: str(constants.get(match.group(0), match.group(0))),
        line,
    )


def eval_asm_expr(expr: str, constants: Dict[str, int]) -> Optional[int]:
    expr = substitute_constants(expr.strip(), constants)
    if not re.fullmatch(r"[0-9xXa-fA-F\s()+\-*/%<>&|~]+", expr):
        return None
    try:
        return int(eval(expr, {"__builtins__": {}}, {}))
    except Exception:
        return None


def format_asm_expr(expr: str, constants: Dict[str, int]) -> str:
    value = eval_asm_expr(expr, constants)
    if value is not None:
        return str(value)
    return substitute_constants(expr, constants)


def eval_macro_condition(expr: str, constants: Dict[str, int]) -> bool:
    expr = substitute_constants(expr, constants)
    expr = expr.replace("||", " or ").replace("&&", " and ")
    expr = re.sub(r"(?<![=!<>])!(?!=)", " not ", expr)
    try:
        return bool(eval(expr, {"__builtins__": {}}, {}))
    except Exception:
        return False


def expand_event_macro(
    name: str,
    args: List[str],
    macros: Dict[str, AsmMacro],
    constants: Dict[str, int],
    counters: Optional[Dict[str, int]] = None,
    depth: int = 0,
) -> Optional[List[str]]:
    if depth > 8 or name not in macros:
        return None

    macro = macros[name]
    values = {}
    positional = []
    for arg in args:
        if "=" in arg:
            key, value = arg.split("=", 1)
            values[key.strip()] = value.strip()
        else:
            positional.append(arg)

    positional_index = 0
    for index, (param, default) in enumerate(macro.params):
        if default == VARARG_DEFAULT:
            values[param] = ", ".join(positional[positional_index:])
            positional_index = len(positional)
            continue
        if param not in values:
            if positional_index < len(positional):
                values[param] = positional[positional_index]
                positional_index += 1
            elif default is not None:
                values[param] = default
            else:
                values[param] = ""

    out: List[str] = []
    conditional_stack: List[Tuple[bool, bool]] = []
    local_label_args: List[str] = []
    for body_line in macro.body:
        stripped = body_line.strip()
        if stripped.startswith((".warning", ".error", ".set", ".purgem")):
            continue
        if stripped.startswith(".ifb "):
            value = values.get(stripped[len(".ifb "):].strip().lstrip("\\"), "")
            conditional_stack.append((not value, not value))
            continue
        if stripped.startswith(".ifnb "):
            value = values.get(stripped[len(".ifnb "):].strip().lstrip("\\"), "")
            conditional_stack.append((bool(value), bool(value)))
            continue
        if stripped.startswith(".if "):
            expr = stripped[len(".if "):].strip()
            for param, value in values.items():
                expr = expr.replace(f"\\{param}", value or "0")
            condition = eval_macro_condition(expr, constants)
            conditional_stack.append((condition, condition))
            continue
        if stripped.startswith(".elseif "):
            if conditional_stack:
                _active, branch_taken = conditional_stack[-1]
                condition = False
                if not branch_taken:
                    expr = stripped[len(".elseif "):].strip()
                    for param, value in values.items():
                        expr = expr.replace(f"\\{param}", value or "0")
                    condition = eval_macro_condition(expr, constants)
                conditional_stack[-1] = (condition, branch_taken or condition)
            continue
        if stripped == ".else":
            if conditional_stack:
                _active, branch_taken = conditional_stack[-1]
                conditional_stack[-1] = (not branch_taken, True)
            continue
        if stripped == ".endif":
            if conditional_stack:
                conditional_stack.pop()
            continue
        if conditional_stack and not all(active for active, _seen_else in conditional_stack):
            continue

        expanded = stripped
        for param, value in sorted(values.items(), key=lambda item: len(item[0]), reverse=True):
            expanded = expanded.replace(f"\\{param}", value)
        expanded = re.sub(r"\\@", str(counters.setdefault("macro_local_id", 0)), expanded)

        if LABEL_RE.match(expanded):
            if local_label_args:
                local_label_args = []
            continue

        if re.search(r"\.L\w+_\d+_2\s*-\s*\.L\w+_\d+_1", expanded):
            out.append(f".byte {len(split_args(values.get('argv', '')))}")
            continue

        if expanded.startswith(".2byte "):
            operands = split_args(expanded[len(".2byte "):])
            local_label_args = operands
            if not operands:
                continue
            out.append(".2byte " + ", ".join(format_asm_expr(operand, constants) for operand in operands))
            continue

        if expanded.startswith((".byte ", ".4byte ")):
            directive, operands_text = expanded.split(" ", 1)
            operands = split_args(operands_text)
            if directive == ".4byte":
                function_symbols = load_function_symbols()
                for operand in operands:
                    if operand in function_symbols:
                        out.append(f".functype {operand} (i32) -> ()")
            out.append(f"{directive} " + ", ".join(format_asm_expr(operand, constants) for operand in operands))
            continue

        if expanded.startswith(".space "):
            out.append(substitute_constants(expanded, constants))
            continue

        nested_name, _, nested_args = expanded.partition(" ")
        if nested_name == "map":
            map_value = parse_int(nested_args.strip(), constants)
            out.extend([f".byte {map_value >> 8}", f".byte {map_value & 0xFF}"])
            continue
        if nested_name == "formatwarp":
            warp_args = split_args(nested_args)
            if warp_args:
                map_value = parse_int(warp_args[0], constants)
                out.extend([f".byte {map_value >> 8}", f".byte {map_value & 0xFF}"])
                if len(warp_args) == 1:
                    out.extend([f".byte {parse_int('WARP_ID_NONE', constants)}", ".2byte -1", ".2byte -1"])
                elif len(warp_args) == 2:
                    out.extend([f".byte {warp_args[1]}", ".2byte -1", ".2byte -1"])
                elif len(warp_args) == 3:
                    out.extend([f".byte {parse_int('WARP_ID_NONE', constants)}", f".2byte {warp_args[1]}", f".2byte {warp_args[2]}"])
                else:
                    out.extend([f".byte {warp_args[1]}", f".2byte {warp_args[2]}", f".2byte {warp_args[3]}"])
                continue

        nested = expand_macro(expanded, constants, counters if counters is not None else {}, macros, depth + 1)
        if nested is None:
            return None
        out.extend(nested)

    return out


def expand_macro(stripped: str, constants: Dict[str, int], counters: Dict[str, int], macros: Dict[str, AsmMacro], macro_depth: int = 0) -> Optional[List[str]]:
    def is_var_id(value: str) -> bool:
        try:
            parsed = parse_int(value, constants)
        except Exception:
            return False
        return (
            constants.get("VARS_START", 0x4000) <= parsed <= constants.get("VARS_END", 0x40FF)
            or constants.get("SPECIAL_VARS_START", 0x8000) <= parsed <= constants.get("SPECIAL_VARS_END", 0x8015)
        )

    def compare_lines(var: str, value: str) -> List[str]:
        if is_var_id(value):
            return [
                f".byte {parse_int('SCR_OP_COMPARE_VAR_TO_VAR', constants)}",
                f".2byte {var}",
                f".2byte {value}",
            ]
        return [
            f".byte {parse_int('SCR_OP_COMPARE_VAR_TO_VALUE', constants)}",
            f".2byte {var}",
            f".2byte {value}",
        ]

    def conditional_lines(command: str, condition: int, args: List[str]) -> List[str]:
        opcode = "SCR_OP_CALL_IF" if command == "call" else "SCR_OP_GOTO_IF"
        if len(args) == 1:
            return [f".byte {parse_int(opcode, constants)}", f".byte {condition}", f".4byte {args[0]}"]
        if len(args) == 3:
            var, value, destination = args
            return [
                *compare_lines(var, value),
                f".byte {parse_int(opcode, constants)}",
                f".byte {condition}",
                f".4byte {destination}",
            ]
        raise ValueError(f"unsupported {command}_if arguments: {args}")

    def special_id(name: str) -> int:
        if name.startswith("SPECIAL_"):
            return parse_int(name, constants)
        return parse_int(f"SPECIAL_{name}", constants)

    def special_waitstate(name: str) -> bool:
        if name.startswith("SPECIAL_"):
            name = name[len("SPECIAL_"):]
        return bool(parse_int(f"SPECIAL_WAITSTATE_{name}", constants))

    def expand_loaded_macro() -> Optional[List[str]]:
        name, _, args = stripped.partition(" ")
        return expand_event_macro(name, parse_macro_args(name, args, macros), macros, constants, counters, macro_depth)

    macro_name = stripped.split(" ", 1)[0]
    # Native macro definitions own command names that collide across script engines.
    if macro_name in macros and (stripped == macro_name or macro_name in {"end", "return", "call", "goto"}):
        expanded = expand_loaded_macro()
        if expanded is not None:
            return expanded

    if stripped.startswith("script_cmd_table_entry "):
        if not constants.get("ALLOCATE_SCRIPT_CMD_TABLE", 0):
            return []
        args = split_args(stripped[len("script_cmd_table_entry "):])
        if len(args) == 1:
            args = args[0].split()
        _constant, function = args[:2]
        return [f".functype {function} (i32) -> (i32)", f".4byte {function}"]

    if stripped.startswith("def_special "):
        if not constants.get("ALLOCATE_SPECIAL_TABLE", 0):
            return []
        special = split_args(stripped[len("def_special "):])[0]
        counters.setdefault("special_wrappers", []).append((special, bool(constants.get(f"SPECIAL_RETURNS_VALUE_{special}", 1))))
        wrapper = f"WasmSpecial_{special}"
        return [f".functype {wrapper} () -> (i32)", f".4byte {wrapper}"]

    if stripped.startswith("map_script "):
        script_type, script = split_args(stripped[len("map_script "):])
        return [f".byte {script_type}", f".4byte {script}"]

    if stripped.startswith("map_script_2 "):
        var, value, script = split_args(stripped[len("map_script_2 "):])
        return [f".2byte {var}", f".2byte {value}", f".4byte {script}"]

    if stripped == "end":
        return [f".byte {parse_int('SCR_OP_END', constants)}"]

    if stripped == "return":
        return [f".byte {parse_int('SCR_OP_RETURN', constants)}"]

    if stripped == "lockall":
        return [f".byte {parse_int('SCR_OP_LOCKALL', constants)}"]

    if stripped == "releaseall":
        return [f".byte {parse_int('SCR_OP_RELEASEALL', constants)}"]

    if stripped == "checkplayergender":
        return [f".byte {parse_int('SCR_OP_CHECKPLAYERGENDER', constants)}"]

    if stripped == "waitstate":
        return [f".byte {parse_int('SCR_OP_WAITSTATE', constants)}"]

    if stripped == "waitmessage":
        return [f".byte {parse_int('SCR_OP_WAITMESSAGE', constants)}"]

    if stripped == "waitbuttonpress":
        return [f".byte {parse_int('SCR_OP_WAITBUTTONPRESS', constants)}"]

    if stripped == "closemessage":
        return [f".byte {parse_int('SCR_OP_CLOSEMESSAGE', constants)}"]

    if stripped == "waitdooranim":
        return [f".byte {parse_int('SCR_OP_WAITDOORANIM', constants)}"]

    if stripped == "lock":
        return [f".byte {parse_int('SCR_OP_LOCK', constants)}"]

    if stripped == "release":
        return [f".byte {parse_int('SCR_OP_RELEASE', constants)}"]

    if stripped == "faceplayer":
        return [f".byte {parse_int('SCR_OP_FACEPLAYER', constants)}"]

    if stripped == "hideplayer":
        return [f".byte {parse_int('SCR_OP_HIDEOBJECTAT', constants)}", ".2byte 255", ".byte 0", ".byte 0"]

    if stripped == "showplayer":
        return [f".byte {parse_int('SCR_OP_SHOWOBJECTAT', constants)}", ".2byte 255", ".byte 0", ".byte 0"]

    if stripped.startswith("call "):
        destination = stripped[len("call "):].strip()
        return [f".byte {parse_int('SCR_OP_CALL', constants)}", f".4byte {destination}"]

    if stripped.startswith("goto "):
        destination = stripped[len("goto "):].strip()
        return [f".byte {parse_int('SCR_OP_GOTO', constants)}", f".4byte {destination}"]

    if stripped.startswith("goto_if "):
        condition, destination = split_args(stripped[len("goto_if "):])
        return [f".byte {parse_int('SCR_OP_GOTO_IF', constants)}", f".byte {condition}", f".4byte {destination}"]

    if stripped.startswith("call_if "):
        condition, destination = split_args(stripped[len("call_if "):])
        return [f".byte {parse_int('SCR_OP_CALL_IF', constants)}", f".byte {condition}", f".4byte {destination}"]

    shorthand = re.match(r"^(goto|call)_if_(lt|eq|gt|le|ge|ne)\s+(.+)$", stripped)
    if shorthand:
        command, condition_name, args_text = shorthand.groups()
        condition = {"lt": 0, "eq": 1, "gt": 2, "le": 3, "ge": 4, "ne": 5}[condition_name]
        return conditional_lines(command, condition, split_args(args_text))

    if stripped.startswith("call_if_unset "):
        flag, destination = split_args(stripped[len("call_if_unset "):])
        return [
            f".byte {parse_int('SCR_OP_CHECKFLAG', constants)}",
            f".2byte {flag}",
            f".byte {parse_int('SCR_OP_CALL_IF', constants)}",
            ".byte 0",
            f".4byte {destination}",
        ]

    if stripped.startswith("call_if_set "):
        flag, destination = split_args(stripped[len("call_if_set "):])
        return [
            f".byte {parse_int('SCR_OP_CHECKFLAG', constants)}",
            f".2byte {flag}",
            f".byte {parse_int('SCR_OP_CALL_IF', constants)}",
            ".byte 1",
            f".4byte {destination}",
        ]

    if stripped.startswith("goto_if_set "):
        flag, destination = split_args(stripped[len("goto_if_set "):])
        return [
            f".byte {parse_int('SCR_OP_CHECKFLAG', constants)}",
            f".2byte {flag}",
            f".byte {parse_int('SCR_OP_GOTO_IF', constants)}",
            ".byte 1",
            f".4byte {destination}",
        ]

    if stripped.startswith("goto_if_unset "):
        flag, destination = split_args(stripped[len("goto_if_unset "):])
        return [
            f".byte {parse_int('SCR_OP_CHECKFLAG', constants)}",
            f".2byte {flag}",
            f".byte {parse_int('SCR_OP_GOTO_IF', constants)}",
            ".byte 0",
            f".4byte {destination}",
        ]

    if stripped.startswith("setmetatile "):
        x, y, metatile, impassable = split_args(stripped[len("setmetatile "):])
        return [f".byte {parse_int('SCR_OP_SETMETATILE', constants)}", f".2byte {x}", f".2byte {y}", f".2byte {metatile}", f".2byte {impassable}"]

    if stripped.startswith("setstepcallback "):
        step = stripped[len("setstepcallback "):].strip()
        return [f".byte {parse_int('SCR_OP_SETSTEPCALLBACK', constants)}", f".byte {step}"]

    if stripped.startswith("setflag "):
        flag = stripped[len("setflag "):].strip()
        return [f".byte {parse_int('SCR_OP_SETFLAG', constants)}", f".2byte {flag}"]

    if stripped.startswith("clearflag "):
        flag = stripped[len("clearflag "):].strip()
        return [f".byte {parse_int('SCR_OP_CLEARFLAG', constants)}", f".2byte {flag}"]

    if stripped.startswith("setrespawn "):
        heal_location = stripped[len("setrespawn "):].strip()
        return [f".byte {parse_int('SCR_OP_SETRESPAWN', constants)}", f".2byte {heal_location}"]

    if stripped.startswith("setvar "):
        var, value = split_args(stripped[len("setvar "):])[:2]
        return [f".byte {parse_int('SCR_OP_SETVAR', constants)}", f".2byte {var}", f".2byte {value}"]

    if stripped.startswith("copyvar "):
        destination, source = split_args(stripped[len("copyvar "):])[:2]
        return [f".byte {parse_int('SCR_OP_COPYVAR', constants)}", f".2byte {destination}", f".2byte {source}"]

    if stripped.startswith("setorcopyvar "):
        destination, source = split_args(stripped[len("setorcopyvar "):])[:2]
        return [f".byte {parse_int('SCR_OP_SETORCOPYVAR', constants)}", f".2byte {destination}", f".2byte {source}"]

    if stripped.startswith("specialvar "):
        var, special = split_args(stripped[len("specialvar "):])[:2]
        lines = [f".byte {parse_int('SCR_OP_SPECIALVAR', constants)}", f".2byte {var}", f".2byte {special_id(special)}"]
        if special_waitstate(special):
            lines.append(f".byte {parse_int('SCR_OP_WAITSTATE', constants)}")
        return lines

    if stripped.startswith("special "):
        special = stripped[len("special "):].strip()
        lines = [f".byte {parse_int('SCR_OP_SPECIAL', constants)}", f".2byte {special_id(special)}"]
        if special_waitstate(special):
            lines.append(f".byte {parse_int('SCR_OP_WAITSTATE', constants)}")
        return lines

    if stripped.startswith("playse "):
        song = stripped[len("playse "):].strip()
        return [f".byte {parse_int('SCR_OP_PLAYSE', constants)}", f".2byte {song}"]

    if stripped.startswith("playfanfare "):
        song = stripped[len("playfanfare "):].strip()
        return [f".byte {parse_int('SCR_OP_PLAYFANFARE', constants)}", f".2byte {song}"]

    if stripped == "waitfanfare":
        return [f".byte {parse_int('SCR_OP_WAITFANFARE', constants)}"]

    if stripped.startswith("setdynamicwarp "):
        args = split_args(stripped[len("setdynamicwarp "):])
        map_value = parse_int(args[0], constants)
        lines = [
            f".byte {parse_int('SCR_OP_SETDYNAMICWARP', constants)}",
            f".byte {map_value >> 8}",
            f".byte {map_value & 0xFF}",
        ]
        if len(args) == 2:
            lines.extend([f".byte {args[1]}", ".2byte -1", ".2byte -1"])
        elif len(args) == 3:
            lines.extend([f".byte {parse_int('WARP_ID_NONE', constants)}", f".2byte {args[1]}", f".2byte {args[2]}"])
        elif len(args) == 4:
            lines.extend([f".byte {args[1]}", f".2byte {args[2]}", f".2byte {args[3]}"])
        else:
            lines.extend([f".byte {parse_int('WARP_ID_NONE', constants)}", ".2byte -1", ".2byte -1"])
        return lines

    if stripped.startswith("warpsilent "):
        args = split_args(stripped[len("warpsilent "):])
        map_value = parse_int(args[0], constants)
        lines = [
            f".byte {parse_int('SCR_OP_WARPSILENT', constants)}",
            f".byte {map_value >> 8}",
            f".byte {map_value & 0xFF}",
        ]
        if len(args) == 2:
            lines.extend([f".byte {args[1]}", ".2byte -1", ".2byte -1"])
        elif len(args) == 3:
            lines.extend([f".byte {parse_int('WARP_ID_NONE', constants)}", f".2byte {args[1]}", f".2byte {args[2]}"])
        elif len(args) == 4:
            lines.extend([f".byte {args[1]}", f".2byte {args[2]}", f".2byte {args[3]}"])
        else:
            lines.extend([f".byte {parse_int('WARP_ID_NONE', constants)}", ".2byte -1", ".2byte -1"])
        return lines

    if stripped.startswith("warp "):
        args = split_args(stripped[len("warp "):])
        map_value = parse_int(args[0], constants)
        lines = [
            f".byte {parse_int('SCR_OP_WARP', constants)}",
            f".byte {map_value >> 8}",
            f".byte {map_value & 0xFF}",
        ]
        if len(args) == 2:
            lines.extend([f".byte {args[1]}", ".2byte -1", ".2byte -1"])
        elif len(args) == 3:
            lines.extend([f".byte {parse_int('WARP_ID_NONE', constants)}", f".2byte {args[1]}", f".2byte {args[2]}"])
        elif len(args) == 4:
            lines.extend([f".byte {args[1]}", f".2byte {args[2]}", f".2byte {args[3]}"])
        else:
            lines.extend([f".byte {parse_int('WARP_ID_NONE', constants)}", ".2byte -1", ".2byte -1"])
        return lines

    if stripped.startswith("msgbox "):
        text, msgbox_type = split_args(stripped[len("msgbox "):])
        return [
            f".byte {parse_int('SCR_OP_LOAD_WORD', constants)}",
            ".byte 0",
            f".4byte {text}",
            f".byte {parse_int('SCR_OP_CALL_STD', constants)}",
            f".byte {msgbox_type}",
        ]

    if stripped.startswith("message "):
        text = stripped[len("message "):].strip()
        return [f".byte {parse_int('SCR_OP_MESSAGE', constants)}", f".4byte {text}"]

    if stripped.startswith("delay "):
        value = stripped[len("delay "):].strip()
        return [f".byte {parse_int('SCR_OP_DELAY', constants)}", f".2byte {value}"]

    if stripped.startswith("applymovement "):
        local_id, movements = split_args(stripped[len("applymovement "):])[:2]
        return [f".byte {parse_int('SCR_OP_APPLYMOVEMENT', constants)}", f".2byte {local_id}", f".4byte {movements}"]

    if stripped.startswith("waitmovement"):
        args = split_args(stripped[len("waitmovement"):].strip())
        local_id = args[0] if args and args[0] else "0"
        return [f".byte {parse_int('SCR_OP_WAITMOVEMENT', constants)}", f".2byte {local_id}"]

    if stripped.startswith("addobject "):
        local_id = split_args(stripped[len("addobject "):])[0]
        return [f".byte {parse_int('SCR_OP_ADDOBJECT', constants)}", f".2byte {local_id}"]

    if stripped.startswith("removeobject "):
        local_id = split_args(stripped[len("removeobject "):])[0]
        return [f".byte {parse_int('SCR_OP_REMOVEOBJECT', constants)}", f".2byte {local_id}"]

    if stripped.startswith("setobjectxyperm "):
        local_id, x, y = split_args(stripped[len("setobjectxyperm "):])
        return [f".byte {parse_int('SCR_OP_SETOBJECTXYPERM', constants)}", f".2byte {local_id}", f".2byte {x}", f".2byte {y}"]

    if stripped.startswith("setobjectxy "):
        local_id, x, y = split_args(stripped[len("setobjectxy "):])
        return [f".byte {parse_int('SCR_OP_SETOBJECTXY', constants)}", f".2byte {local_id}", f".2byte {x}", f".2byte {y}"]

    if stripped.startswith("setobjectmovementtype "):
        local_id, movement_type = split_args(stripped[len("setobjectmovementtype "):])
        return [f".byte {parse_int('SCR_OP_SETOBJECTMOVEMENTTYPE', constants)}", f".2byte {local_id}", f".byte {movement_type}"]

    if stripped.startswith("setdooropen "):
        x, y = split_args(stripped[len("setdooropen "):])
        return [f".byte {parse_int('SCR_OP_SETDOOROPEN', constants)}", f".2byte {x}", f".2byte {y}"]

    if stripped.startswith("setdoorclosed "):
        x, y = split_args(stripped[len("setdoorclosed "):])
        return [f".byte {parse_int('SCR_OP_SETDOORCLOSED', constants)}", f".2byte {x}", f".2byte {y}"]

    if stripped.startswith("opendoor "):
        x, y = split_args(stripped[len("opendoor "):])
        return [f".byte {parse_int('SCR_OP_OPENDOOR', constants)}", f".2byte {x}", f".2byte {y}"]

    if stripped.startswith("closedoor "):
        x, y = split_args(stripped[len("closedoor "):])
        return [f".byte {parse_int('SCR_OP_CLOSEDOOR', constants)}", f".2byte {x}", f".2byte {y}"]

    if stripped.startswith("hideobjectat "):
        local_id, map_id = split_args(stripped[len("hideobjectat "):])
        map_value = parse_int(map_id, constants)
        return [f".byte {parse_int('SCR_OP_HIDEOBJECTAT', constants)}", f".2byte {local_id}", f".byte {map_value >> 8}", f".byte {map_value & 0xFF}"]

    if stripped in constants:
        return [f".byte {parse_int(stripped, constants)}"]

    if stripped == "reset_map_events":
        counters.update(npcs=0, warps=0, traps=0, signs=0)
        return []

    if stripped.startswith("object_event "):
        args = split_args(stripped[len("object_event "):])
        counters["npcs"] += 1
        return [
            f".byte {args[0]}",
            f".byte {args[1]}",
            f".byte {parse_int('OBJ_KIND_NORMAL', constants)}",
            ".space 1",
            f".2byte {args[2]}, {args[3]}",
            f".byte {args[4]}",
            f".byte {args[5]}",
            f".byte ((({args[7]}) << 4) | ({args[6]}))",
            ".space 1",
            f".2byte {args[8]}",
            f".2byte {args[9]}",
            f".4byte {args[10]}",
            f".2byte {args[11]}",
            ".space 2",
        ]

    if stripped.startswith("warp_def "):
        x, y, elevation, warp_id, map_id = split_args(stripped[len("warp_def "):])
        map_value = parse_int(map_id, constants)
        counters["warps"] += 1
        return [
            f".2byte {x}, {y}",
            f".byte {elevation}",
            f".byte {warp_id}",
            f".byte {map_value & 0xFF}",
            f".byte {map_value >> 8}",
        ]

    if stripped.startswith("coord_weather_event "):
        x, y, elevation, weather = split_args(stripped[len("coord_weather_event "):])
        stripped = f"coord_event {x}, {y}, {elevation}, {weather}, 0, 0"

    if stripped.startswith("coord_event "):
        x, y, elevation, var, var_value, script = split_args(stripped[len("coord_event "):])
        counters["traps"] += 1
        return [
            f".2byte {x}, {y}",
            f".byte {elevation}",
            ".space 1",
            f".2byte {var}",
            f".2byte {var_value}",
            ".space 2",
            f".4byte {script}",
        ]

    if stripped.startswith("bg_sign_event "):
        x, y, elevation, facing_dir, script = split_args(stripped[len("bg_sign_event "):])
        stripped = f"bg_event {x}, {y}, {elevation}, {facing_dir}, {script}"

    if stripped.startswith("bg_hidden_item_event "):
        x, y, elevation, item, flag = split_args(stripped[len("bg_hidden_item_event "):])
        hidden_item = parse_int("BG_EVENT_HIDDEN_ITEM", constants)
        flag_start = parse_int("FLAG_HIDDEN_ITEMS_START", constants)
        stripped = f"bg_event {x}, {y}, {elevation}, {hidden_item}, {item}, (({flag}) - {flag_start})"

    if stripped.startswith("bg_secret_base_event "):
        x, y, elevation, secret_base_id = split_args(stripped[len("bg_secret_base_event "):])
        secret_base = parse_int("BG_EVENT_SECRET_BASE", constants)
        stripped = f"bg_event {x}, {y}, {elevation}, {secret_base}, {secret_base_id}"

    if stripped.startswith("bg_event "):
        args = split_args(stripped[len("bg_event "):])
        x, y, elevation, kind, arg6 = args[:5]
        kind_value = parse_int(kind, constants)
        counters["signs"] += 1
        lines = [
            f".2byte {x}, {y}",
            f".byte {elevation}",
            f".byte {kind}",
            ".space 2",
        ]
        if kind_value == parse_int("BG_EVENT_HIDDEN_ITEM", constants):
            lines.extend([f".2byte {arg6}", f".2byte {args[5]}"])
        else:
            lines.append(f".4byte {arg6}")
        return lines

    if stripped.startswith("map_events "):
        npcs, warps, traps, signs = split_args(stripped[len("map_events "):])
        lines = [
            f".byte {counters['npcs']}, {counters['warps']}, {counters['traps']}, {counters['signs']}",
            f".4byte {npcs}, {warps}, {traps}, {signs}",
        ]
        counters.update(npcs=0, warps=0, traps=0, signs=0)
        return lines

    if stripped.startswith("map_header_flags "):
        values = {}
        for arg in split_args(stripped[len("map_header_flags "):]):
            key, value = arg.split("=", 1)
            values[key.strip()] = parse_int(value, constants)
        byte = (
            ((values["show_map_name"] & 1) << 3)
            | ((values["allow_running"] & 1) << 2)
            | ((values["allow_escaping"] & 1) << 1)
            | (values["allow_cycling"] & 1)
        )
        return [f".byte {byte}"]

    if stripped.startswith("connection "):
        direction, offset, map_id = split_args(stripped[len("connection "):])
        connection_id = constants[f"connection_{direction}"]
        map_value = parse_int(map_id, constants)
        return [
            f".byte {connection_id}",
            ".space 3",
            f".4byte {parse_int(offset, constants)}",
            f".byte {map_value >> 8}",
            f".byte {map_value & 0xFF}",
            ".space 2",
        ]

    if stripped.startswith("map "):
        map_value = parse_int(stripped[len("map "):], constants)
        return [f".byte {map_value >> 8}", f".byte {map_value & 0xFF}"]

    expanded = expand_loaded_macro()
    if expanded is not None:
        return expanded

    return None


def strip_at_comment(line: str) -> str:
    in_quote = False
    escaped = False
    for i, char in enumerate(line):
        if escaped:
            escaped = False
        elif char == "\\":
            escaped = True
        elif char == '"':
            in_quote = not in_quote
        elif char in "@;" and not in_quote:
            return line[:i].rstrip()
    return line.rstrip()


def convert(text: str, source: Optional[Path] = None, emit_sizes: bool = True) -> str:
    out = []
    constants = load_script_command_constants()
    script_command_functions = set(load_script_command_functions())
    function_symbols = load_function_symbols()
    constants.update(load_special_constants())
    macros = load_event_macros(source)
    constants.update(load_movement_constants())
    constants.update(load_map_constants())
    constants.update(load_source_constants(source))
    constants.update({
        "OBJ_KIND_NORMAL": 0,
        "OBJ_KIND_CLONE": 1,
        "BG_EVENT_HIDDEN_ITEM": 7,
        "BG_EVENT_SECRET_BASE": 8,
        "FLAG_HIDDEN_ITEMS_START": 0x1F4,
        "NULL": 0,
        "FALSE": 0,
        "TRUE": 1,
        "MSGBOX_SIGN": 3,
        "MSGBOX_DEFAULT": 4,
        "STEP_CB_TRUCK": 5,
        "SE_LEDGE": 10,
        "LOCALID_PLAYER": 255,
        "LOCALID_NONE": 0,
        "MOVEMENT_ACTION_STEP_END": 0xFE,
    })
    counters = {"npcs": 0, "warps": 0, "traps": 0, "signs": 0}
    open_label = None
    open_label_sized = False
    conditional_stack: List[bool] = []

    def close_label():
        nonlocal open_label, open_label_sized
        if open_label_sized and open_label:
            out.append(f".size {open_label}, .-{open_label}")
        open_label = None
        open_label_sized = False

    skip_macro = 0
    for raw in text.splitlines():
        is_global_label = "; .global" in raw
        line = strip_at_comment(raw)
        if not line:
            continue

        stripped = line.strip()
        if stripped.startswith("#"):
            continue
        if stripped.startswith(".if "):
            expr = stripped[len(".if "):].strip()
            try:
                conditional_stack.append(bool(eval(expr, {"__builtins__": {}}, constants)))
            except Exception:
                conditional_stack.append(True)
            continue
        if stripped.startswith(".ifndef "):
            name = stripped[len(".ifndef "):].strip()
            conditional_stack.append(name not in constants)
            continue
        if stripped == ".else":
            if conditional_stack:
                conditional_stack[-1] = not conditional_stack[-1]
            continue
        if stripped == ".endif":
            if conditional_stack:
                conditional_stack.pop()
            continue
        if conditional_stack and not all(conditional_stack):
            continue
        if skip_macro:
            if stripped.startswith(".macro "):
                skip_macro += 1
            elif stripped == ".endm":
                skip_macro -= 1
            continue
        if stripped.startswith(".macro "):
            skip_macro = 1
            continue
        if (
            stripped.startswith("enum_start ")
            or stripped == "enum_start"
            or stripped.startswith("enum ")
            or stripped.startswith("create_movement_action ")
            or stripped in {"reset_map_events", "inc _num_npcs", "inc _num_warps", "inc _num_traps", "inc _num_signs"}
        ):
            continue
        assignment = ASSIGN_RE.match(stripped)
        if stripped.startswith(".equiv ") or stripped.startswith(".set "):
            name, expr = re.split(r"\s+", stripped, maxsplit=1)[1].split(",", 1)
            try:
                constants[name.strip()] = eval(expr, {"__builtins__": {}}, constants)
            except Exception:
                pass
            continue
        if assignment:
            name, expr = assignment.groups()
            try:
                constants[name] = eval(expr, {"__builtins__": {}}, constants)
            except Exception:
                pass
            continue

        if constants:
            stripped = substitute_constants(stripped, constants)

        macro = expand_macro(stripped, constants, counters, macros)
        if macro is not None:
            out.extend(macro)
            continue

        if stripped.startswith(".section"):
            close_label()
            section = stripped.split()[1].split(",", 1)[0]
            out.append(f".section {section},\"\",@")
            continue
        if stripped.startswith((".if", ".else", ".endif", ".ifndef", ".purgem")):
            continue

        match = LABEL_RE.match(stripped)
        if match:
            name, colons, rest = match.groups()
            close_label()
            global_label = colons == "::" or is_global_label
            if global_label:
                out.append(f".globl {name}")
            out.append(f".type {name},@object")
            out.append(f"{name}:")
            open_label = name
            open_label_sized = emit_sizes or global_label
            if rest:
                out.append(rest)
            continue

        if stripped.startswith(".4byte "):
            symbol = stripped[len(".4byte "):].strip()
            if symbol in script_command_functions:
                out.append(f".functype {symbol} (i32) -> (i32)")
            elif symbol in function_symbols:
                out.append(f".functype {symbol} (i32) -> ()")
        out.append(stripped)

    close_label()
    special_wrappers = counters.get("special_wrappers", [])
    if special_wrappers:
        emitted_wrappers = set()
        for special, returns_value in special_wrappers:
            if special in emitted_wrappers:
                continue
            emitted_wrappers.add(special)
            wrapper = f"WasmSpecial_{special}"
            return_type = "i32" if returns_value else ""
            out.extend([
                f'.section .text.{wrapper},"",@',
                f".functype {special} () -> ({return_type})",
                f".globl {wrapper}",
                f".type {wrapper},@function",
                f"{wrapper}:",
                f".functype {wrapper} () -> (i32)",
                f"call {special}",
            ])
            if not returns_value:
                out.append("i32.const 0")
            out.append("end_function")
    return "\n".join(out) + "\n"


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: wasm_asm_data.py <source.s> <output.s>")
    source = Path(sys.argv[1])
    output = Path(sys.argv[2])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(convert(preprocess(source), source))


if __name__ == "__main__":
    main()
