#!/usr/bin/env python3
"""Adapt explicit GBA address casts in preprocessed C for native pointers."""
import json
import re
import subprocess
import sys
from pathlib import Path

source, output, target_format, *compiler = sys.argv[1:]
text = Path(source).read_text()
# Preprocessing has already resolved macros; offsets must refer to this file.
text = '\n'.join(line for line in text.splitlines() if not line.startswith('#')) + '\n'
section = '__DATA,native_game' if target_format == 'macho' else 'native_game'
text = re.sub(r'__attribute__\(\(section\("(?:iwram_data|ewram_data|common_data|\.bss\.code|\.text\.consts)"\)\)\)', '', text)
text = f'#pragma clang section bss="{section}" data="{section}_data"\n' + '#pragma pack(4)\n' + text
def parse(text):
    Path(source).write_text(text)
    return subprocess.run([*compiler, '-x', 'c', '-fsyntax-only', '-Wno-everything',
                           '-Xclang', '-ast-dump=json', source], capture_output=True)


def packed_records(node):
    if node.get('kind') == 'RecordDecl' and any(
            child.get('kind') == 'PackedAttr' for child in node.get('inner', [])):
        bounds = node['range']
        yield bounds['begin']['offset'], bounds['end']['offset'] + bounds['end']['tokLen']
    for child in node.get('inner', []):
        yield from packed_records(child)


result = parse(text)
# Clang's pack(4) raises the alignment of packed records to four. Preserve
# their original byte alignment, including packed records nested in structs.
packing = []
for start, end in packed_records(json.loads(result.stdout)):
    # Include trailing attributes and declarators before restoring alignment.
    end = text.index(';', end) + 1
    packing.extend([(start, '\n#pragma pack(push, 1)\n'),
                    (end, '\n#pragma pack(pop)\n')])
for offset, insertion in sorted(set(packing), reverse=True):
    text = text[:offset] + insertion + text[offset:]
if packing:
    result = parse(text)
if result.returncode:
    sys.stderr.buffer.write(result.stderr)
    raise SystemExit(result.returncode)
root = json.loads(result.stdout)
edits = []

def visit(node):
    kind = node.get('castKind')
    if node.get('kind') == 'CStyleCastExpr' and kind in ('PointerToIntegral', 'IntegralToPointer'):
        child = node['inner'][0]
        if kind == 'IntegralToPointer' and node.get('inner', [{}])[0].get('value') == '0':
            return
        bounds = child['range']
        start = bounds['begin']['offset']
        end = bounds['end']['offset'] + bounds['end']['tokLen']
        func = 'NativePointerToWord' if kind == 'PointerToIntegral' else 'NativeWordToPointer'
        edits.append((start, end, func))
    dereference = None
    if node.get('kind') == 'MemberExpr' and node.get('isArrow'):
        dereference = node['inner'][0]
    elif node.get('kind') == 'UnaryOperator' and node.get('opcode') == '*':
        dereference = node['inner'][0]
    elif node.get('kind') == 'ArraySubscriptExpr':
        candidate = node['inner'][0]
        if candidate.get('castKind') != 'ArrayToPointerDecay':
            dereference = candidate
    if dereference:
        bounds = dereference['range']
        start = bounds['begin']['offset']
        end = bounds['end']['offset'] + bounds['end']['tokLen']
        edits.append((start, end, 'NativeDereferencePointer'))
    for child in node.get('inner', []):
        visit(child)

visit(root)
insertions = {}
for start, end, func in sorted(edits, key=lambda edit: (edit[0], -edit[1])):
    insertions.setdefault(start, [0, []])[1].append(func + '(')
    insertions.setdefault(end, [0, []])[0] += 1
for offset, (closes, opens) in sorted(insertions.items(), reverse=True):
    text = text[:offset] + ')' * closes + ''.join(opens) + text[offset:]
Path(output).write_text('extern unsigned char gNativeMemory[];\n'
                        '#define NativeDereferencePointer(pointer) ((pointer) ?: (__typeof__(1 ? (pointer) : (pointer)))gNativeMemory)\n'
                        'extern unsigned int NativePointerToWord(const void *);\n'
                        'extern void *NativeDecodePointer(unsigned long);\n'
                        '#define NativeWordToPointer(word) (__builtin_constant_p(word) && (unsigned long)(word) < 0x10000000ul ? (void *)(gNativeMemory + (unsigned long)(word)) : NativeDecodePointer(word))\n' + text)
