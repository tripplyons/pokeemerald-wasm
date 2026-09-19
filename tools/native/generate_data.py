#!/usr/bin/env python3
"""Emit native data and 32-bit script relocations from the shared ASM expander."""
import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import wasm_asm_data as asm


def pointer_tables():
    tables = {'gScriptCmdTable', 'gScriptCmdTableEnd', 'gSpecials'}
    for directory in ('src', 'include'):
        for path in Path(directory).rglob('*.[ch]'):
            for match in re.finditer(r'extern\s+([^;{}]+?)\b(\w+)\s*\[[^;{}]*;', path.read_text()):
                if '*' in match[1] or 'Func' in match[1]:
                    tables.add(match[2])
    return tables


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--pointer-size', type=int, choices=(4, 8), required=True)
    parser.add_argument('--format', choices=('macho', 'elf'), required=True)
    args = parser.parse_args()
    source = asm.preprocess(args.source)
    source = re.sub(r'(?m)^\s*enum_start(?:[ \t]+([^\n@]+))?[ \t]*$',
                    lambda match: '.set __enum__, ' + (match[1] or '0'), source)
    source = re.sub(r'(?m)^[ \t]*enum[ \t]+(\w+)[ \t]*$',
                    r'.equiv \1, __enum__\n.set __enum__, __enum__ + 1', source)
    converted = asm.convert(source, args.source)
    # Special wrappers are C functions in native builds.
    data_text, _, wrappers = converted.partition('.section .text.')
    globals = set(re.findall(r'(?m)^\.globl (\w+)$', data_text))
    labels = []
    data = bytearray()
    relocations = {}
    for raw in data_text.splitlines():
        line = raw.strip()
        if line.endswith(':'):
            labels.append((line[:-1], len(data)))
        elif line.startswith(('.byte ', '.2byte ', '.4byte ')):
            directive, operands = line.split(' ', 1)
            width = {'.byte': 1, '.2byte': 2, '.4byte': 4}[directive]
            for operand in asm.split_args(operands):
                try:
                    value = int(eval(operand, {'__builtins__': {}}, {}))
                    data.extend((value & ((1 << (width * 8)) - 1)).to_bytes(width, 'little'))
                except (NameError, SyntaxError):
                    if width != 4:
                        raise ValueError(f'non-word relocation: {line}')
                    relocations[len(data)] = operand
                    data.extend(bytes(4))
        elif line.startswith('.space '):
            operands = asm.split_args(line[7:])
            data.extend(bytes([int(operands[1], 0) if len(operands) > 1 else 0]) * int(operands[0], 0))
        elif line.startswith(('.align ', '.balign ')):
            directive, value = line.split(' ', 1)
            alignment = int(value, 0)
            if directive == '.align':
                alignment = 1 << alignment
            data.extend(bytes((-len(data)) % alignment))
        elif line.startswith('.incbin '):
            data.extend(Path(json.loads(line[8:])).read_bytes())
        elif line.startswith(('.section ', '.size ', '.type ', '.globl ', '.global ', '.functype ')) or not line:
            pass
        else:
            raise ValueError(f'unsupported native data directive: {line}')

    tables = pointer_tables()
    map_names = {json.loads(path.read_text())['name'] for path in Path('data/maps').glob('*/map.json')}
    # These records have four-byte packing, matching the native C preparation.
    # Fields listed here contain a C pointer, rather than a script address word.
    def pointer_fields(name, length):
        if name in tables or name.startswith('gMapGroup') or name == 'gMapLayouts':
            return set(range(0, length, 4))
        if args.source.stem == 'maps':
            if name in map_names:
                return {0, 4, 8, 12}
            if name.endswith('_Layout'):
                return {8, 12, 16, 20}
            if name.endswith('_MapConnections'):
                return {4}
        if args.source.stem == 'map_events':
            for suffix, stride, offsets in (
                ('_ObjectEvents', 24, (16,)), ('_MapCoordEvents', 16, (12,)),
                ('_MapBGEvents', 12, (8,)), ('_MapEvents', 20, (4, 8, 12, 16)),
            ):
                if name.endswith(suffix):
                    return {base + offset for base in range(0, length, stride) for offset in offsets}
        return set()

    blob = bytearray()
    native_labels = []
    patches = []
    for index, (name, start) in enumerate(labels):
        end = labels[index + 1][1] if index + 1 < len(labels) else len(data)
        native_labels.append((name, len(blob)))
        fields = pointer_fields(name, end - start)
        pos = start
        while pos < end:
            wide = pos - start in fields
            if pos in relocations:
                patches.append((len(blob), relocations[pos], wide))
                blob.extend(bytes(args.pointer_size if wide else 4))
                pos += 4
            elif wide:
                blob.extend(data[pos:pos + 4])
                blob.extend(bytes(args.pointer_size - 4))
                pos += 4
            else:
                blob.append(data[pos])
                pos += 1

    prefix = '_' if args.format == 'macho' else ''
    blob_name = 'NativeData_' + args.source.stem
    out = ['#include <stdint.h>', '#include <string.h>',
           'extern uint32_t NativePointerToWord(const void *);',
           f'unsigned char {blob_name}[] __attribute__((aligned(8))) = {{']
    out.extend('    ' + ','.join(str(value) for value in blob[i:i + 32]) + ',' for i in range(0, len(blob), 32))
    out.append('};')
    local_labels = {name: offset for name, offset in native_labels if name not in globals}
    patches = [(offset, re.sub(r'\b[A-Za-z_]\w*\b',
                lambda match: f'({blob_name} + {local_labels[match[0]]})' if match[0] in local_labels else match[0],
                expression), wide) for offset, expression, wide in patches]
    for name, offset in native_labels:
        if name not in globals:
            continue
        out.append('__asm__(' + json.dumps(f'.globl {prefix}{name}\n{prefix}{name} = {prefix}{blob_name} + {offset}') + ');')
    symbols = set()
    for _, expression, _ in patches:
        symbols.update(re.findall(r'\b[A-Za-z_]\w*\b', expression))
    for symbol in sorted(symbols - {blob_name}):
        out.append(f'extern unsigned char {symbol}[];')
    out.append(f'void NativeInit_{args.source.stem}(void) {{')
    for offset, expression, wide in patches:
        if wide:
            out.append(f'    {{ const void *p = (const void *)({expression}); memcpy({blob_name} + {offset}, &p, sizeof(p)); }}')
        else:
            out.append(f'    {{ uint32_t p = NativePointerToWord((const void *)({expression})); memcpy({blob_name} + {offset}, &p, sizeof(p)); }}')
    out.append('}')
    if wrappers:
        for name, returns_value in asm.load_special_return_values().items():
            wrapper = 'WasmSpecial_' + name
            if wrapper not in symbols:
                continue
            # The table declarations above only need the symbol address. Use an
            # assembly name to avoid giving one C name both object and function types.
            out.append(f'extern {"unsigned int" if returns_value else "void"} {name}(void);')
            out.append(f'unsigned int NativeSpecial_{name}(void) __asm__("{prefix}{wrapper}");')
            out.append(f'unsigned int NativeSpecial_{name}(void) {{ {"return " if returns_value else ""}{name}(); {"" if returns_value else "return 0;"} }}')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
